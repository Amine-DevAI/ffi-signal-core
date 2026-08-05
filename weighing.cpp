#include "weighing.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Heap-allocates a C string copy for FFI transfer.
// Dart receives the pointer and MUST free it with weighing_free_string().
// Returns nullptr if the input string is empty.
static char *weighing_alloc(const std::string &s) {
  if (s.empty())
    return nullptr;
  char *p = static_cast<char *>(std::malloc(s.length() + 1));
  if (p)
    std::memcpy(p, s.c_str(), s.length() + 1);
  return p;
}

// Resolves and validates the context pointer.
// Returns nullptr if handle is null or db is not ready.
static WasteTrackingContext *resolve_ctx(WasteTrackingHandle handle) {
  if (!handle)
    return nullptr;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->db)
    return nullptr;
  return ctx;
}

// Permission gate — resolves the result string from checkPermission.
// Returns true only when the session is alive and the role is allowed.
static bool weighing_check(WasteTrackingHandle handle, UserHandle user_handle,
                           const std::string &permission) {
  if (!handle || !user_handle)
    return false;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return false;
  const char *result = ctx->auth->checkPermission(user_handle, permission);
  return (std::strcmp(result, "OK") == 0);
}

// Maps permission check result to the correct WasteTrackingError.
static WasteTrackingError permission_error(WasteTrackingHandle handle,
                                           UserHandle user_handle,
                                           const std::string &permission) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return WT_ERROR_INVALID_STATE;
  const char *result = ctx->auth->checkPermission(user_handle, permission);
  if (std::strcmp(result, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(result, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(result, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;
  return WT_ERROR_PERMISSION;
}

// ============================================================================
// WEIGHING FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// weighing_create
//
// OPERATOR / ADMIN only.
// Validates lot requirement client-side before hitting the server.
// Builds the exact payload the server expects — field names must match
// the server's /api/weigh/create endpoint exactly:
//   weight (net), weight_gross, weight_tare, operator_id,
//   type_id, product_id, lot_number
// Server now generates the QR code (W-YYMMDD-XXXX) and returns it.
// Returns the QR code string — Dart uses it to print the label.
// ---------------------------------------------------------------------------
char *weighing_create(WasteTrackingHandle handle, UserHandle user_handle,
                      int32_t type_id,
                      int32_t product_id, // Removed int32_t zone_id
                      double gross_weight, double tare_weight,
                      const char *lot_number) {

  if (!weighing_check(handle, user_handle, "create_weighing")) {
    std::cerr << "[Weighing] weighing_create denied — permission check failed."
              << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  if (type_id <= 0) {
    std::cerr << "[Weighing] weighing_create — type_id is required."
              << std::endl;
    return nullptr;
  }

  if (gross_weight <= 0.0 || tare_weight < 0.0) {
    std::cerr << "[Weighing] weighing_create — invalid weight values."
              << std::endl;
    return nullptr;
  }

  // Client-side lot validation — avoids a round trip on failure.
  // Server re-validates this as a second gate (defense in depth).
  json type_obj = ctx->db->api_types_Get(type_id);
  if (!type_obj.is_null() && type_obj.value("requires_lot", false)) {
    if (!lot_number || std::strlen(lot_number) == 0) {
      std::cerr << "[Weighing] weighing_create — type " << type_id
                << " requires a lot number." << std::endl;
      return nullptr;
    }
  }

  // Net weight: gross minus tare, floored at zero.
  // Server recomputes this from the trio — we send all three.
  double net_weight =
      (gross_weight > tare_weight) ? (gross_weight - tare_weight) : 0.0;

  // User ID comes directly from the live session — never from Dart input.
  auto *s = static_cast<UserSession *>(user_handle);
  int32_t op_id = s->user_id;

  // Build the payload exactly as the server expects it.
  // No uuid field — server generates the QR code (W-YYMMDD-XXXX).
  json payload;
  payload["type_id"] = type_id;
  payload["operator_id"] = op_id;
  payload["weight"] = net_weight;         // server column: weight (net)
  payload["weight_gross"] = gross_weight; // server column: weight_gross
  payload["weight_tare"] = tare_weight;   // server column: weight_tare

  // Optional fields — only include if meaningful
  if (product_id > 0)
    payload["product_id"] = product_id;

  if (lot_number && std::strlen(lot_number) > 0)
    payload["lot_number"] = std::string(lot_number);

  // api_weigh_Create now returns the full response object with id and qr_code
  json response = ctx->db->api_weigh_Create(payload);

  if (response.is_null()) {
    std::cerr << "[Weighing] weighing_create — server rejected the record."
              << std::endl;
    return nullptr;
  }

  // Extract the QR code from the response
  std::string qr_code = response.value("qr_code", "");
  int32_t new_id = response.value("id", -1);

  if (new_id < 0 || qr_code.empty()) {
    std::cerr << "[Weighing] weighing_create — invalid response from server."
              << std::endl;
    return nullptr;
  }

  std::cout << "[Weighing] weighing_create OK — id=" << new_id
            << " qr_code=" << qr_code << " net=" << net_weight << "kg"
            << std::endl;

  // Return QR code — Dart encodes this in the physical label.
  return weighing_alloc(qr_code);
}

// ---------------------------------------------------------------------------
// weighing_get_by_uuid
//
// Any valid session — view_data permission.
// Used when operator or validator scans a QR on the floor.
// UUID is the primary lookup key — the server queries by qr_code_data.
// ---------------------------------------------------------------------------
char *weighing_get_by_uuid(WasteTrackingHandle handle, UserHandle user_handle,
                           const char *uuid) {

  if (!weighing_check(handle, user_handle, "view_data")) {
    std::cerr << "[Weighing] weighing_get_by_uuid denied — permission check "
                 "failed."
              << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  if (!uuid || std::strlen(uuid) == 0) {
    std::cerr << "[Weighing] weighing_get_by_uuid — uuid is required."
              << std::endl;
    return nullptr;
  }

  json result = ctx->db->api_weigh_GetByUUID(std::string(uuid));

  if (result.is_null()) {
    std::cerr << "[Weighing] weighing_get_by_uuid — not found: " << uuid
              << std::endl;
    return nullptr;
  }

  return weighing_alloc(result.dump());
}

// ---------------------------------------------------------------------------
// weighing_my_list
//
// Any valid session — view_data permission.
// Returns the calling operator's last 50 weighings.
// user_id is taken from the live session — Dart never provides it.
// Server scopes results to this user only — operators are isolated.
// ---------------------------------------------------------------------------
char *weighing_my_list(WasteTrackingHandle handle, UserHandle user_handle) {

  if (!weighing_check(handle, user_handle, "view_data")) {
    std::cerr << "[Weighing] weighing_my_list denied — permission check failed."
              << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  auto *s = static_cast<UserSession *>(user_handle);

  json list = ctx->db->api_weigh_MyList(s->user_id);

  if (!list.is_array()) {
    std::cerr
        << "[Weighing] weighing_my_list — unexpected response from server."
        << std::endl;
    return nullptr;
  }

  return weighing_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// weighing_correct
//
// OPERATOR / ADMIN / VALIDATOR / COORDINATEUR — correct_record permission.
// Server atomically:
//   1. Updates the weighing fields in changes_json
//   2. Recalculates net weight from updated gross and tare
//   3. Logs the correction in the corrections table
//   4. Raises a flag in the flags table (admin will see it)
//   5. Sets is_flagged = TRUE on the weighing record
//
// Dart builds changes_json with only the fields being corrected.
// reason is mandatory — server rejects without it.
// ---------------------------------------------------------------------------
WasteTrackingError weighing_correct(WasteTrackingHandle handle,
                                    UserHandle user_handle, int32_t weighing_id,
                                    const char *reason,
                                    const char *changes_json) {

  if (!weighing_check(handle, user_handle, "correct_record"))
    return permission_error(handle, user_handle, "correct_record");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (weighing_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  if (!reason || std::strlen(reason) == 0) {
    std::cerr << "[Weighing] weighing_correct — reason is required."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // Parse the changes JSON — must be a valid object
  json changes;
  if (changes_json && std::strlen(changes_json) > 0) {
    try {
      changes = json::parse(std::string(changes_json));
    } catch (...) {
      std::cerr << "[Weighing] weighing_correct — invalid changes_json."
                << std::endl;
      return WT_ERROR_INVALID_PARAM;
    }
  } else {
    // No changes provided — nothing to correct
    std::cerr << "[Weighing] weighing_correct — changes_json is empty."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // user_id from the live session — never from Dart input
  auto *s = static_cast<UserSession *>(user_handle);

  bool ok = ctx->db->api_weigh_Correct(weighing_id, s->user_id,
                                       std::string(reason), changes);

  if (!ok) {
    std::cerr << "[Weighing] weighing_correct — server rejected correction for "
                 "weighing_id="
              << weighing_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Weighing] weighing_correct OK — weighing_id=" << weighing_id
            << " by user_id=" << s->user_id << std::endl;

  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// weighing_free_string
// ---------------------------------------------------------------------------
void weighing_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ---------------------------------------------------------------------------
// weighing_set_notify_callback
//
// Dart calls this once after login, before the socket connects.
// Stores the callback on UserSession — survives socket rebuilds.
// Dart MUST use NativeCallable.permanent() to prevent GC collection.
// ---------------------------------------------------------------------------
void weighing_set_notify_callback(UserHandle user_handle,
                                  WeighingNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Weighing] weighing_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->weighing_notify_cb = cb;
  std::cout << "[Weighing] Notify callback registered for user " << s->user_id
            << std::endl;
}

// ---------------------------------------------------------------------------
// notify_weighing
//
// Called by the socket chamber when WEIGHING_CORRECTED arrives.
// Dart receives "WEIGHING_CORRECTED" and calls weighing_my_list() to refresh.
// NEW_FLAG_ALERT is NOT routed here — that goes to admin chamber only.
// Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void notify_weighing(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Weighing] notify_weighing — session dead, dropping: "
              << signal_name << std::endl;
    return;
  }

  if (!s->weighing_notify_cb) {
    std::cerr << "[Weighing] notify_weighing — no callback registered, "
                 "dropping: "
              << signal_name << std::endl;
    return;
  }

  std::cout << "[Weighing] → Dart: " << signal_name << std::endl;
  s->weighing_notify_cb(signal_name);
}

} // extern "C"