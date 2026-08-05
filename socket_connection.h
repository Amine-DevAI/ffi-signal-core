#ifndef SOCKET_CONNECTION_H
#define SOCKET_CONNECTION_H

#include "types.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <ixwebsocket/IXWebSocket.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// ============================================================================
// SocketConnection
//
// Owns the four WebSocket connections for one user session:
//
//   /ws/signals        — receives WORKFLOW_SIGNAL envelopes from the server.
//                        Each category is dispatched to the correct notify_*()
//                        function. This is the only routing that happens here.
//
//   /ws/heartbeat      — sends PING every 40s, receives PONG / IDENTIFIED /
//                        FORCE_LOGOUT. Manages the heartbeat thread lifecycle.
//
//   /ws/bridge         — QR scan relay for the reconciliation desk.
//                        Registers this desktop as a station on connect,
//                        receives INCOMING_SCAN and fires cb_reco_scan_.
//
//   /ws/bridge/shipment — same pattern, independent switchboard for shipment.
//
// Design contract:
//   - This class does NO business logic. It receives a message, identifies its
//     type, and calls the right function. That's it.
//   - All notify_*() functions are called with the UserHandle that was stored
//     at connect_all() time. The chambers own those functions — this class
//     just knows their names.
//   - The only callbacks stored here are the two QR scan callbacks and the
//     two internal lambdas (IDENTIFIED, FORCE_LOGOUT) that auth.cpp needs to
//     update UserSession fields before firing the Dart callback. Everything
//     else goes through notify_*().
//   - Thread safety: sockets run on their own IXWebSocket threads. The
//     heartbeat thread is the only thread we own. The mutex_ guard covers
//     connect/disconnect only — message handlers are reentrant by design.
//
// Lifecycle:
//   1. Construct (zero state, no threads, no connections).
//   2. Set the two scan callbacks if needed (before connect_all).
//   3. Set the two internal lambdas if needed (auth.cpp does this at login).
//   4. connect_all() — starts all sockets and the heartbeat thread.
//   5. disconnect_all() — stops everything. Safe to call from any thread,
//      including from inside a socket message callback (won't deadlock).
//   6. Destructor calls disconnect_all() automatically.
// ============================================================================

class SocketConnection {
public:
  SocketConnection();
  ~SocketConnection();

  // --- Public getters for reconnect logic ---
  int getUserId() const { return user_id_; }
  const std::string &getToken() const { return token_; }
  UserHandle getUserHandle() const { return user_handle_; }

  // Non-copyable, non-movable — pointer stability required by IXWebSocket
  SocketConnection(const SocketConnection &) = delete;
  SocketConnection &operator=(const SocketConnection &) = delete;

  // -------------------------------------------------------------------------
  // Connection control
  // -------------------------------------------------------------------------

  // Stores user_id, token, and user_handle, then starts all four sockets and
  // the heartbeat thread. Safe to call once per session — calling again while
  // already connected is a no-op (returns true).
  // user_handle is stored as a raw pointer for the session lifetime.
  // It is NOT owned here — it is owned by the UserSession / auth chamber.
  bool connect_all(const std::string &base_url, int user_id,
                   const std::string &token, UserHandle user_handle);

  // Signals the heartbeat thread to stop, then stops all four sockets.
  // Safe to call from any thread, including from inside a socket callback.
  // Idempotent — safe to call multiple times.
  void disconnect_all();

  bool isConnected() const { return is_connected_.load(); }

  // -------------------------------------------------------------------------
  // Scan callbacks — set BEFORE connect_all()
  // These are the only plain C function pointers stored here.
  // Everything else routes through notify_*() using the stored UserHandle.
  // -------------------------------------------------------------------------
  void onRecoScan(ScanCallback cb) { cb_reco_scan_ = cb; }
  void onShipScan(ScanCallback cb) { cb_ship_scan_ = cb; }
  void onDenatScan(ScanCallback cb) { cb_denat_scan_ = cb; }

  // -------------------------------------------------------------------------
  // Internal lambdas — used by auth.cpp at login time
  // These need to capture UserSession* to update station_id and fire the
  // Dart callback. Plain function pointers can't do that — these are the
  // only std::function members. Everything else uses notify_*().
  // -------------------------------------------------------------------------
  void onIdentified(std::function<void(const char *, const char *)> cb) {
    cb_identified_ = std::move(cb);
  }
  void onForceLogout(std::function<void(const char *)> cb) {
    cb_force_logout_ = std::move(cb);
  }

private:
  // -------------------------------------------------------------------------
  // Socket setup — called once inside connect_all()
  // -------------------------------------------------------------------------
  void setupSignalSocket();
  void setupHeartbeatSocket();
  void setupRecoBridgeSocket();
  void setupShipBridgeSocket();
  void setupDenatBridgeSocket();

  // -------------------------------------------------------------------------
  // Dispatch — the only routing logic in this file
  // -------------------------------------------------------------------------

  // Receives a parsed WORKFLOW_SIGNAL envelope from /ws/signals.
  // Reads "category", calls the matching notify_*() function.
  // Never does anything else.
  void dispatchSignal(const json &envelope);

  // Receives a parsed message from /ws/heartbeat.
  // Handles IDENTIFIED, PONG, FORCE_LOGOUT only.
  void dispatchHeartbeat(const json &msg);

  // -------------------------------------------------------------------------
  // Heartbeat thread
  // -------------------------------------------------------------------------
  void heartbeatLoop();
  void sendIdentify(ix::WebSocket &ws);
  static bool isFatalClose(const std::string &reason);

  // -------------------------------------------------------------------------
  // Sockets
  // -------------------------------------------------------------------------
  ix::WebSocket signal_socket_;
  ix::WebSocket heartbeat_socket_;
  ix::WebSocket reco_socket_;
  ix::WebSocket ship_socket_;
  ix::WebSocket denat_socket_;

  // -------------------------------------------------------------------------
  // Session state — set at connect_all(), read-only after that
  // -------------------------------------------------------------------------
  int user_id_ = -1;
  std::string token_;
  UserHandle user_handle_ = nullptr; // NOT owned here
  std::string base_url_;

  // -------------------------------------------------------------------------
  // Heartbeat thread control
  // -------------------------------------------------------------------------
  std::atomic<bool> is_connected_{false};
  std::atomic<bool> stop_heartbeat_{false};
  std::atomic<bool> heartbeat_running_{false};
  std::atomic<int> missed_pongs_{0};
  std::mutex hb_mtx_;
  std::condition_variable hb_cv_;

  // -------------------------------------------------------------------------
  // Callbacks
  // Plain C pointers — QR scan relay only
  // -------------------------------------------------------------------------
  ScanCallback cb_reco_scan_ = nullptr;
  ScanCallback cb_ship_scan_ = nullptr;
  ScanCallback cb_denat_scan_ = nullptr;

  // std::function — auth.cpp capturing lambdas only
  std::function<void(const char *, const char *)> cb_identified_;
  std::function<void(const char *)> cb_force_logout_;

  // General connection mutex — protects connect/disconnect sequencing
  std::mutex connect_mtx_;
};

// Returns a stable station fingerprint from /etc/machine-id
std::string get_station_id();

#endif // SOCKET_CONNECTION_H