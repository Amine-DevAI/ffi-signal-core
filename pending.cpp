#include "pending.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static char *pending_alloc(const std::string &s) {
  if (s.empty())
    return nullptr;
  char *p = static_cast<char *>(std::malloc(s.length() + 1));
  if (p)
    std::memcpy(p, s.c_str(), s.length() + 1);
  return p;
}

static WasteTrackingContext *resolve_ctx(WasteTrackingHandle handle) {
  if (!handle)
    return nullptr;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->db)
    return nullptr;
  return ctx;
}

static bool pending_check(WasteTrackingHandle handle, UserHandle user_handle,
                          const std::string &permission) {
  if (!handle || !user_handle)
    return false;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return false;
  return std::strcmp(ctx->auth->checkPermission(user_handle, permission),
                     "OK") == 0;
}

// Always returns JSON — never nullptr on reachable server.
static char *error_json(const std::string &code,
                        const std::string &detail = "") {
  std::string j = "{\"success\":false,\"error\":\"" + code + "\"";
  if (!detail.empty())
    j += ",\"detail\":\"" + detail + "\"";
  j += "}";
  return pending_alloc(j);
}

// ============================================================================
// PENDING FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// pending_reco_list
// Validator's live work queue — all pending and scanned reconciliations.
// Server returns them FIFO (oldest first) — fair queue for the factory floor.
// Contains everything the validator needs: weights, material, product,
// lot, QR, is_flagged.
// ---------------------------------------------------------------------------
char *pending_reco_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!pending_check(handle, user_handle, "accept_reco"))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  json list = ctx->db->api_reco_Pending();

  if (!list.is_array()) {
    std::cerr
        << "[Pending] pending_reco_list — unexpected response from server."
        << std::endl;
    return error_json("SERVER_ERROR");
  }

  std::cout << "[Pending] Reco queue: " << list.size() << " items."
            << std::endl;

  json response = {{"success", true}, {"data", list}};
  return pending_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// pending_ship_list
// Shipper's live work queue — all approved shipments waiting to be loaded.
// Server groups them by zone so the shipper can work through the warehouse
// systematically without jumping between zones.
// ---------------------------------------------------------------------------
char *pending_ship_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!pending_check(handle, user_handle, "dispatch_shipment"))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  json list = ctx->db->api_ship_Pending();

  if (!list.is_array()) {
    std::cerr
        << "[Pending] pending_ship_list — unexpected response from server."
        << std::endl;
    return error_json("SERVER_ERROR");
  }

  std::cout << "[Pending] Ship queue: " << list.size() << " items."
            << std::endl;

  json response = {{"success", true}, {"data", list}};
  return pending_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// pending_denat_list (THE REINFORCED VERSION)
//
// Coordinateur's live work queue.
// Logic:
//  1. Check permissions (Denaturation role required).
//  2. Resolve context (Database connection check).
//  3. Fetch Mega-Join JSON from DB layer.
//  4. Guard against non-array responses to protect Dart loops.
//  5. Alloc memory on the heap for FFI delivery.
// ---------------------------------------------------------------------------
char *pending_denat_list(WasteTrackingHandle handle, UserHandle user_handle) {
  // 1. Perimeter Security: Role-Based Access Control
  if (!pending_check(handle, user_handle, "denaturation")) {
    std::cerr
        << "[Pending] Security Breach: User not authorized for denat queue."
        << std::endl;
    return error_json("PERMISSION_DENIED");
  }

  // 2. Systems Check: Ensure Database Context is alive
  auto *ctx = resolve_ctx(handle);
  if (!ctx) {
    return error_json("NO_CONTEXT",
                      "WasteTrackingHandle is invalid or db null");
  }

  try {
    // 3. The Extraction: Hit the Mega-Join endpoint
    // This pulls: Material Nature, Product Brand, Operator Name, Zone, Lot, and
    // Physics.
    json list = ctx->db->api_denat_ListPending();

    // 4. Data Integrity Guard: Ensure we return an array, even if empty
    if (!list.is_array()) {
      std::cerr << "[Pending] Data Mismatch: Expected array from "
                   "api_denat_ListPending"
                << std::endl;
      return error_json("SERVER_ERROR", "Database returned malformed payload");
    }

    // 5. Package for Export: Wrap in success object
    json response = {{"success", true},
                     {"count", list.size()}, // Meta-data for the Chamber to log
                     {"data", list}};

    std::cout << "[Pending] Denat queue synchronized: " << list.size()
              << " targets identified." << std::endl;

    // 6. Deliver to FFI: Transfer ownership to the heap
    return pending_alloc(response.dump());

  } catch (const std::exception &e) {
    // Ultimate Safety Net
    std::cerr << "[Pending] CRITICAL FAILURE: " << e.what() << std::endl;
    return error_json("INTERNAL_EXCEPTION", e.what());
  }
}

// ---------------------------------------------------------------------------
// MEMORY
// ---------------------------------------------------------------------------

void pending_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ---------------------------------------------------------------------------
// NOTIFICATION
// ---------------------------------------------------------------------------

void pending_set_notify_callback(UserHandle user_handle,
                                 PendingNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Pending] pending_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->pending_notify_cb = cb;
  std::cout << "[Pending] Notify callback registered for user " << s->user_id
            << std::endl;
}

void notify_pending(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Pending] notify_pending — session dead, dropping: "
              << signal_name << std::endl;
    return;
  }

  if (!s->pending_notify_cb) {
    std::cerr << "[Pending] notify_pending — no callback registered, dropping: "
              << signal_name << std::endl;
    return;
  }

  std::cout << "[Pending] → Dart: " << signal_name << std::endl;
  s->pending_notify_cb(signal_name);
}

} // extern "C"