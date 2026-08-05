#include "shipment.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static char *ship_alloc(const std::string &s) {
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

static bool ship_check(WasteTrackingHandle handle, UserHandle user_handle,
                       const std::string &permission) {
  if (!handle || !user_handle)
    return false;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return false;
  return std::strcmp(ctx->auth->checkPermission(user_handle, permission),
                     "OK") == 0;
}

static WasteTrackingError permission_error(WasteTrackingHandle handle,
                                           UserHandle user_handle,
                                           const std::string &permission) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return WT_ERROR_INVALID_STATE;
  const char *r = ctx->auth->checkPermission(user_handle, permission);
  if (std::strcmp(r, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(r, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  return WT_ERROR_PERMISSION;
}

// Builds a structured error JSON string — never nullptr.
// Dart always gets something it can parse.
static char *error_json(const std::string &code,
                        const std::string &detail = "") {
  std::string j = "{\"success\":false,\"error\":\"" + code + "\"";
  if (!detail.empty())
    j += ",\"detail\":\"" + detail + "\"";
  j += "}";
  return ship_alloc(j);
}

// ============================================================================
// SHIPMENT FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// ship_get_by_qr
// Shipper scans QR → gets full batch detail to confirm before dispatching.
// Server returns verified weights, material, product, lot, zone.
// Always returns JSON — never nullptr on reachable server.
// ---------------------------------------------------------------------------
char *ship_get_by_qr(WasteTrackingHandle handle, UserHandle user_handle,
                     const char *qr_data) {
  if (!ship_check(handle, user_handle, "dispatch_shipment"))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  if (!qr_data || std::strlen(qr_data) == 0)
    return error_json("INVALID_PARAM", "QR data is empty");

  json result = ctx->db->api_ship_GetByQR(std::string(qr_data));

  if (result.is_null()) {
    // Server returns nullptr on 404 or 409 (already dispatched)
    // database layer already logged the reason
    return error_json("NOT_FOUND", "No pending shipment found for this QR");
  }

  // Pass through server data exactly —
  // fields: shipment_id, weights{net,gross,tare}, material, product, lot, zone
  json response = {{"success", true}, {"data", result}};
  return ship_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// ship_dispatch
// Confirms the batch is loaded. Server does atomic triple-sync.
// Denaturation guard built into the server SQL — blocks if not completed.
// Returns JSON with flagged_warning so Dart can show a warning banner.
// ---------------------------------------------------------------------------
char *ship_dispatch(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t shipment_id, const char *note) {
  if (!ship_check(handle, user_handle, "dispatch_shipment"))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  if (shipment_id <= 0)
    return error_json("INVALID_PARAM", "shipment_id must be positive");

  // user_id comes from the live session — never from Dart input
  auto *s = static_cast<UserSession *>(user_handle);

  // note is optional — use default if not provided
  std::string note_str =
      (note && std::strlen(note) > 0) ? std::string(note) : "Loaded onto truck";

  // api_ship_Dispatch returns bool — but we need the flagged_warning data.
  // The server returns {shipment_id, flagged_warning} in its response body.
  // database.cpp api_ship_Dispatch only returns bool — we lose flagged_warning.
  // So we call http layer directly through the existing bool return and build
  // a minimal response. flagged_warning is a nice-to-have — dispatch success
  // is what matters. Dart can always re-fetch the record if it needs the flag.
  json result =
      ctx->db->api_ship_Dispatch(shipment_id, s->user_id, "shipped", note_str);

  if (result.is_null()) {
    std::cerr << "[Ship] ship_dispatch failed — shipment_id=" << shipment_id
              << " — may be blocked by pending denaturation." << std::endl;
    return error_json("DENAT_INCOMPLETE",
                      "Shipment blocked — denaturation not completed or "
                      "shipment already processed");
  }

  // Now we read flagged_warning from what the server actually said
  bool was_flagged = result.value("flagged_warning", false);

  if (was_flagged) {
    std::cerr << "[Ship] WARNING — dispatching a flagged shipment: "
              << shipment_id << std::endl;
  }

  std::cout << "[Ship] ship_dispatch OK — shipment_id=" << shipment_id
            << " by user_id=" << s->user_id << std::endl;

  json response = {
      {"success", true},
      {"data",
       {{"shipment_id", shipment_id}, {"flagged_warning", was_flagged}}}};
  return ship_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// ship_my_list
// Shipper's own dispatch history — shipped and cancelled.
// Server scopes by session user_id — shippers only see their own records.
// Each item has full detail for the popup card.
// ---------------------------------------------------------------------------
char *ship_my_list(WasteTrackingHandle handle, UserHandle user_handle,
                   const char *status) {
  if (!ship_check(handle, user_handle, "view_data"))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  auto *s = static_cast<UserSession *>(user_handle);

  std::string status_str =
      (status && std::strlen(status) > 0) ? std::string(status) : "all";

  json list = ctx->db->api_ship_MyList(s->user_id, status_str);

  if (!list.is_array()) {
    std::cerr << "[Ship] ship_my_list — unexpected response from server."
              << std::endl;
    return error_json("SERVER_ERROR");
  }

  json response = {{"success", true}, {"data", list}};
  return ship_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// ship_correct_status
// Shipper clicks "Correct" on a history card.
// Server updates status + logs correction in audit trail atomically.
// new_status must be "pending", "shipped", or "cancelled".
// reason is mandatory — ALCOA+ compliance.
// ---------------------------------------------------------------------------
WasteTrackingError ship_correct_status(WasteTrackingHandle handle,
                                       UserHandle user_handle,
                                       int32_t shipment_id,
                                       const char *new_status,
                                       const char *reason) {
  if (!ship_check(handle, user_handle, "dispatch_shipment"))
    return permission_error(handle, user_handle, "dispatch_shipment");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (shipment_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  if (!new_status || std::strlen(new_status) == 0)
    return WT_ERROR_INVALID_PARAM;

  // Validate status against server CHECK constraint
  std::string status_str(new_status);
  if (status_str != "pending" && status_str != "shipped" &&
      status_str != "cancelled") {
    std::cerr << "[Ship] ship_correct_status — invalid status: " << status_str
              << ". Must be pending, shipped, or cancelled." << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // Reason is mandatory — every correction needs an audit trail
  if (!reason || std::strlen(reason) == 0) {
    std::cerr << "[Ship] ship_correct_status — reason is required."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // user_id from the live session — never from Dart input
  auto *s = static_cast<UserSession *>(user_handle);

  bool ok = ctx->db->api_ship_CorrectStatus(shipment_id, s->user_id, status_str,
                                            std::string(reason));

  if (!ok) {
    std::cerr << "[Ship] ship_correct_status failed — shipment_id="
              << shipment_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Ship] ship_correct_status OK — shipment_id=" << shipment_id
            << " new_status=" << status_str << " by user_id=" << s->user_id
            << std::endl;

  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// MEMORY
// ---------------------------------------------------------------------------

void ship_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ---------------------------------------------------------------------------
// NOTIFICATION
// ---------------------------------------------------------------------------

void ship_set_notify_callback(UserHandle user_handle, ShipNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Ship] ship_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->ship_notify_cb = cb;
  std::cout << "[Ship] Notify callback registered for user " << s->user_id
            << std::endl;
}

void notify_ship(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Ship] notify_ship — session dead, dropping: " << signal_name
              << std::endl;
    return;
  }

  if (!s->ship_notify_cb) {
    std::cerr << "[Ship] notify_ship — no callback registered, dropping: "
              << signal_name << std::endl;
    return;
  }

  std::cout << "[Ship] → Dart: " << signal_name << std::endl;
  s->ship_notify_cb(signal_name);
}

} // extern "C"
