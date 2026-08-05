#include "socket_connection.h"

// Chamber notify functions — this file calls them, it does not own them.
// Each is implemented in its own chamber .cpp file.
#include "admin.h"
#include "auth.h"
#include "catalog.h"
#include "denaturation.h"
#include "pending.h"
#include "reconciliation.h"
#include "shipment.h"
#include "weighing.h"
#include "zone.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

// ============================================================================
// Station fingerprint
// ============================================================================

std::string get_station_id() {
  std::string id;
  std::ifstream f("/etc/machine-id");
  if (!f.is_open())
    f.open("/var/lib/dbus/machine-id");
  if (f && (f >> id))
    return "STATION-" + id.substr(0, 8);

  // Fallback: stable for the process lifetime
  static std::string fallback;
  if (fallback.empty()) {
    std::random_device rd;
    fallback = "DEV-" + std::to_string(rd() % 100000);
  }
  return fallback;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

SocketConnection::SocketConnection() {}

SocketConnection::~SocketConnection() { disconnect_all(); }

// ============================================================================
// Helpers
// ============================================================================

void SocketConnection::sendIdentify(ix::WebSocket &ws) {
  if (user_id_ == -1 || token_.empty())
    return;
  json msg = {{"type", "IDENTIFY"},
              {"user_id", user_id_},
              {"session_token", token_},
              {"station_id", get_station_id()}};
  ws.send(msg.dump());
}

bool SocketConnection::isFatalClose(const std::string &reason) {
  return (reason == "Invalid Session" || reason == "Unauthorized" ||
          reason == "Session deactivated by Admin" ||
          reason == "Session replaced" || reason == "Heartbeat Timeout");
}

// ============================================================================
// Signal dispatch — THE only routing logic
//
// This function receives a WORKFLOW_SIGNAL envelope from /ws/signals.
// It reads "category" and calls the matching notify_*() with the stored
// UserHandle. That is all it does. No business logic lives here.
//
// Signal → chamber mapping (mirrors server emit_signal() calls):
//
//   Auth / user management
//     FORCE_LOGOUT          → auth.handleSignal()
//     USER_CREATED          → auth.handleSignal() + notify_admin()
//     USER_UPDATED          → auth.handleSignal() + notify_admin()
//     USER_DEACTIVATED      → auth.handleSignal() + notify_admin()
//
//   Catalog
//     TYPE_CREATED          → notify_catalog()
//     TYPE_UPDATED          → notify_catalog()
//     TYPE_DELETED          → notify_catalog()
//     REFRESH_PRODUCTS      → notify_catalog()
//
//   Zone
//     REFRESH_ZONES         → notify_zone()
//
//   Weighing
//     WEIGHING_CORRECTED    → notify_weighing()
//
//   Reconciliation
//     RECO_STEP_COMPLETED   → notify_reco() + notify_pending()
//     RECO_REJECTED         → notify_reco()
//     RECO_STATE_UPDATED    → notify_reco()
//
//   Shipment
//     SHIPMENT_READY        → notify_ship() + notify_pending()
//     SHIPMENT_DISPATCHED   → notify_ship()
//
//   Denaturation
//     DENATURATION_PENDING  → notify_denat() + notify_pending()
//     DENATURATION_SUCCESS  → notify_denat()
//
//   Admin
//     NEW_FLAG_ALERT        → notify_admin()
//     FLAG_RESOLVED         → notify_admin()
// ============================================================================

void SocketConnection::dispatchSignal(const json &envelope) {
  if (!user_handle_)
    return;

  std::string category = envelope.value("category", "");
  const json &data =
      envelope.contains("data") ? envelope["data"] : json::object();

  if (category.empty()) {
    std::cerr << "[Socket] dispatchSignal: empty category, dropping."
              << std::endl;
    return;
  }

  std::cout << "[Socket] Signal: " << category << std::endl;

  // --- Auth / user management ---
  if (category == "FORCE_LOGOUT" || category == "USER_CREATED" ||
      category == "USER_UPDATED" || category == "USER_DEACTIVATED") {

    auto *s = static_cast<UserSession *>(user_handle_);
    if (s && s->auth_manager)
      s->auth_manager->handleSignal(user_handle_, category, data);

    // Admin also needs to know about user changes
    if (category != "FORCE_LOGOUT")
      notify_admin(user_handle_, category.c_str());
    return;
  }

  // --- Catalog ---
  if (category == "TYPE_CREATED" || category == "TYPE_UPDATED" ||
      category == "TYPE_DELETED" || category == "REFRESH_PRODUCTS") {
    notify_catalog(user_handle_, category.c_str());
    return;
  }

  // --- Zone ---
  if (category == "REFRESH_ZONES") {
    notify_zone(user_handle_, category.c_str());
    return;
  }

  // --- Weighing ---
  if (category == "WEIGHING_CORRECTED") {
    notify_weighing(user_handle_, category.c_str());
    notify_admin(user_handle_, category.c_str());
    return;
  }

  // --- Reconciliation ---
  if (category == "RECO_STEP_COMPLETED") {
    notify_reco(user_handle_, category.c_str());
    notify_pending(user_handle_, category.c_str());
    return;
  }
  if (category == "RECO_REJECTED" || category == "RECO_STATE_UPDATED") {
    notify_reco(user_handle_, category.c_str());
    notify_admin(user_handle_, category.c_str());
    return;
  }

  // --- Shipment ---
  if (category == "SHIPMENT_READY") {
    notify_ship(user_handle_, category.c_str());
    notify_pending(user_handle_, category.c_str());
    return;
  }
  if (category == "SHIPMENT_DISPATCHED") {
    notify_ship(user_handle_, category.c_str());
    notify_admin(user_handle_, category.c_str());
    return;
  }

  // --- Denaturation ---
  if (category == "DENATURATION_PENDING") {
    notify_denat(user_handle_, category.c_str());
    notify_pending(user_handle_, category.c_str());
    return;
  }
  if (category == "DENATURATION_SUCCESS") {
    notify_denat(user_handle_, category.c_str());
    return;
  }

  // --- Admin only ---
  if (category == "NEW_FLAG_ALERT" || category == "FLAG_RESOLVED") {
    notify_admin(user_handle_, category.c_str());
    return;
  }

  // Unrecognised — log and drop
  std::cerr << "[Socket] Unrecognised signal category: " << category
            << " — dropping." << std::endl;
}

// ============================================================================
// Heartbeat dispatch — handles IDENTIFIED, PONG, FORCE_LOGOUT
// ============================================================================

void SocketConnection::dispatchHeartbeat(const json &msg) {
  std::string type = msg.value("type", "");

  if (type == "PONG") {
    if (missed_pongs_ > 0)
      missed_pongs_--;
    return;
  }

  if (type == "IDENTIFIED") {
    std::string sid = msg.value("station_confirmed", "");
    std::string name = msg.value("user_display_name", "");
    missed_pongs_ = 0;
    std::cout << "[Heartbeat] Identified — station=" << sid << " name=" << name
              << std::endl;
    if (cb_identified_)
      cb_identified_(sid.c_str(), name.c_str());
    return;
  }

  if (type == "FORCE_LOGOUT") {
    std::string reason = msg.value("reason", "Session terminated");
    std::cerr << "[Heartbeat] FORCE_LOGOUT: " << reason << std::endl;
    // Stop the heartbeat thread first — before any callback
    stop_heartbeat_ = true;
    hb_cv_.notify_all();
    // Fire the callback — auth chamber handles the rest
    // We do NOT call disconnect_all() here — that would deadlock
    // because we are inside a socket message callback
    if (cb_force_logout_)
      cb_force_logout_(reason.c_str());
    return;
  }
}

// ============================================================================
// Heartbeat thread
//
// Runs independently of the socket callbacks.
// Sends PING every 40 seconds.
// If 3 consecutive PINGs go unanswered, sends IDENTIFY again —
// the server may have lost our session after a TCP drop.
// Exits cleanly when stop_heartbeat_ is set.
// ============================================================================

void SocketConnection::heartbeatLoop() {
  std::cerr << "[Heartbeat] Thread started for user " << user_id_ << std::endl;

  while (true) {
    // Wait 40 seconds or until woken by disconnect
    {
      std::unique_lock<std::mutex> lk(hb_mtx_);
      hb_cv_.wait_for(lk, std::chrono::seconds(40),
                      [this] { return stop_heartbeat_.load(); });
    }

    if (stop_heartbeat_)
      break;

    if (heartbeat_socket_.getReadyState() != ix::ReadyState::Open) {
      std::cerr << "[Heartbeat] Socket not open — skipping ping." << std::endl;
      continue;
    }

    // 3 missed PONGs → server may have lost us — re-identify
    if (missed_pongs_ >= 3) {
      std::cerr << "[Heartbeat] 3 missed PONGs — re-identifying." << std::endl;
      sendIdentify(heartbeat_socket_);
      missed_pongs_ = 0;
    }

    json ping = {{"type", "PING"}, {"user_id", user_id_}};
    heartbeat_socket_.send(ping.dump());
    missed_pongs_++;

    std::cerr << "[Heartbeat] PING sent — missed_pongs=" << missed_pongs_.load()
              << std::endl;
  }

  std::cerr << "[Heartbeat] Thread exiting." << std::endl;
  heartbeat_running_ = false;
}

// ============================================================================
// Socket setup
// Each socket has exactly one responsibility.
// ============================================================================

void SocketConnection::setupSignalSocket() {
  signal_socket_.setPingInterval(25);
  signal_socket_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          // Identify immediately — server won't accept signals until we do
          sendIdentify(signal_socket_);
          return;
        }

        if (msg->type == ix::WebSocketMessageType::Close) {
          std::cerr << "[Signal] Closed — code=" << msg->closeInfo.code
                    << " reason=" << msg->closeInfo.reason << std::endl;
          if (isFatalClose(msg->closeInfo.reason))
            signal_socket_.stop();
          return;
        }

        if (msg->type != ix::WebSocketMessageType::Message)
          return;

        try {
          auto j = json::parse(msg->str);
          std::string type = j.value("type", "");

          if (type == "SIGNAL_READY") {
            std::cout << "[Signal] Channel ready." << std::endl;
            return;
          }

          if (type == "WORKFLOW_SIGNAL") {
            dispatchSignal(j);
            return;
          }

          std::cerr << "[Signal] Unknown message type: " << type << std::endl;

        } catch (const std::exception &e) {
          std::cerr << "[Signal] Parse error: " << e.what() << std::endl;
        }
      });
}

void SocketConnection::setupHeartbeatSocket() {
  heartbeat_socket_.setPingInterval(25);
  heartbeat_socket_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          std::cerr << "[Heartbeat] Socket opened — sending IDENTIFY."
                    << std::endl;
          sendIdentify(heartbeat_socket_);
          return;
        }

        if (msg->type == ix::WebSocketMessageType::Close) {
          std::cerr << "[Heartbeat] Closed — code=" << msg->closeInfo.code
                    << " reason=" << msg->closeInfo.reason << std::endl;
          if (isFatalClose(msg->closeInfo.reason)) {
            stop_heartbeat_ = true;
            hb_cv_.notify_all();
            heartbeat_socket_.stop();
          }
          return;
        }

        if (msg->type == ix::WebSocketMessageType::Error) {
          std::cerr << "[Heartbeat] Error: " << msg->errorInfo.reason
                    << std::endl;
          return;
        }

        if (msg->type != ix::WebSocketMessageType::Message)
          return;

        try {
          auto j = json::parse(msg->str);
          dispatchHeartbeat(j);
        } catch (const std::exception &e) {
          std::cerr << "[Heartbeat] Parse error: " << e.what() << std::endl;
        }
      });
}

void SocketConnection::setupRecoBridgeSocket() {
  reco_socket_.setPingInterval(25);
  reco_socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      // Register this desktop as a station on the reco bridge
      json reg = {{"type", "REGISTER_DESKTOP"},
                  {"stationId", get_station_id()}};
      reco_socket_.send(reg.dump());
      std::cout << "[RecoBridge] Registered station: " << get_station_id()
                << std::endl;
      return;
    }

    if (msg->type == ix::WebSocketMessageType::Close) {
      if (isFatalClose(msg->closeInfo.reason))
        reco_socket_.stop();
      return;
    }

    if (msg->type != ix::WebSocketMessageType::Message)
      return;

    try {
      auto j = json::parse(msg->str);
      if (j.value("type", "") == "INCOMING_SCAN" && cb_reco_scan_) {
        // data can be a raw string or a nested object depending on relay path
        std::string qr;
        if (j["data"].is_string())
          qr = j["data"].get<std::string>();
        else
          qr = j["data"].dump();
        cb_reco_scan_(qr.c_str());
      }
    } catch (const std::exception &e) {
      std::cerr << "[RecoBridge] Parse error: " << e.what() << std::endl;
    }
  });
}

void SocketConnection::setupShipBridgeSocket() {
  ship_socket_.setPingInterval(25);
  ship_socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      json reg = {{"type", "REGISTER_DESKTOP"},
                  {"stationId", get_station_id()}};
      ship_socket_.send(reg.dump());
      std::cout << "[ShipBridge] Registered station: " << get_station_id()
                << std::endl;
      return;
    }

    if (msg->type == ix::WebSocketMessageType::Close) {
      if (isFatalClose(msg->closeInfo.reason))
        ship_socket_.stop();
      return;
    }

    if (msg->type != ix::WebSocketMessageType::Message)
      return;

    try {
      auto j = json::parse(msg->str);
      if (j.value("type", "") == "INCOMING_SCAN" && cb_ship_scan_) {
        std::string qr;
        if (j["data"].is_string())
          qr = j["data"].get<std::string>();
        else
          qr = j["data"].dump();
        cb_ship_scan_(qr.c_str());
      }
    } catch (const std::exception &e) {
      std::cerr << "[ShipBridge] Parse error: " << e.what() << std::endl;
    }
  });
}

void SocketConnection::setupDenatBridgeSocket() {
  denat_socket_.setPingInterval(25);
  denat_socket_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          // Register this desktop as a station on the denat bridge
          json reg = {{"type", "REGISTER_DESKTOP"},
                      {"stationId", get_station_id()}};
          denat_socket_.send(reg.dump());
          std::cout << "[DenatBridge] Registered station: " << get_station_id()
                    << std::endl;
          return;
        }

        if (msg->type == ix::WebSocketMessageType::Close) {
          if (isFatalClose(msg->closeInfo.reason))
            denat_socket_.stop();
          return;
        }

        if (msg->type != ix::WebSocketMessageType::Message)
          return;

        try {
          auto j = json::parse(msg->str);
          if (j.value("type", "") == "INCOMING_SCAN" && cb_denat_scan_) {
            std::string qr;
            if (j["data"].is_string())
              qr = j["data"].get<std::string>();
            else
              qr = j["data"].dump();
            cb_denat_scan_(qr.c_str());
          }
        } catch (const std::exception &e) {
          std::cerr << "[DenatBridge] Parse error: " << e.what() << std::endl;
        }
      });
}

// ============================================================================
// connect_all
// ============================================================================

bool SocketConnection::connect_all(const std::string &base_url, int user_id,
                                   const std::string &token,
                                   UserHandle user_handle) {
  std::lock_guard<std::mutex> lock(connect_mtx_);

  if (is_connected_) {
    std::cerr
        << "[Socket] connect_all called while already connected — ignoring."
        << std::endl;
    return true;
  }

  // Store session state
  user_id_ = user_id;
  token_ = token;
  user_handle_ = user_handle;
  base_url_ = base_url;

  // Build ws:// or wss:// URL prefix
  std::string prefix =
      (base_url.find("https") != std::string::npos) ? "wss://" : "ws://";
  std::string host = base_url;
  auto sep = host.find("://");
  if (sep != std::string::npos)
    host = host.substr(sep + 3);
  if (!host.empty() && host.back() == '/')
    host.pop_back();

  // Setup handlers before starting — IXWebSocket callbacks are set once
  setupSignalSocket();
  setupHeartbeatSocket();
  setupRecoBridgeSocket();
  setupShipBridgeSocket();
  setupDenatBridgeSocket();

  // Set URLs
  signal_socket_.setUrl(prefix + host + "/ws/signals");
  heartbeat_socket_.setUrl(prefix + host + "/ws/heartbeat");
  reco_socket_.setUrl(prefix + host + "/ws/bridge");
  ship_socket_.setUrl(prefix + host + "/ws/bridge/shipment");
  denat_socket_.setUrl(prefix + host + "/ws/bridge/denaturation");

  // IXWebSocket auto-reconnects — correct for factory floor environments
  signal_socket_.start();
  heartbeat_socket_.start();
  reco_socket_.start();
  ship_socket_.start();
  denat_socket_.start();

  // Start the heartbeat thread exactly once per session
  bool expected = false;
  if (heartbeat_running_.compare_exchange_strong(expected, true)) {
    stop_heartbeat_ = false;
    missed_pongs_ = 0;
    std::thread([this] { heartbeatLoop(); }).detach();
  }

  is_connected_ = true;

  std::cout << "[Socket] Connected — user=" << user_id
            << " station=" << get_station_id() << std::endl;

  return true;
}

// ============================================================================
// disconnect_all
//
// Safe to call from any thread, including from inside a socket message
// callback. Does NOT spin-wait on the heartbeat thread — that thread
// will exit on its own after stop_heartbeat_ is set.
// ============================================================================

void SocketConnection::disconnect_all() {
  // Wake and stop the heartbeat thread first
  stop_heartbeat_ = true;
  hb_cv_.notify_all();

  // Stop all sockets — IXWebSocket::stop() is thread-safe
  signal_socket_.stop();
  heartbeat_socket_.stop();
  reco_socket_.stop();
  ship_socket_.stop();
  denat_socket_.stop();

  is_connected_ = false;

  std::cout << "[Socket] Disconnected — user=" << user_id_ << std::endl;
}