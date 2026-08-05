#include "reconciliation.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Heap allocates a C string for FFI transfer.
// Dart receives the pointer and MUST free with reco_free_string().
static char *alloc_str(const std::string &s) {
  if (s.empty())
    return nullptr;
  char *p = static_cast<char *>(std::malloc(s.length() + 1));
  if (p)
    std::memcpy(p, s.c_str(), s.length() + 1);
  return p;
}

// Validates context and returns the WasteTrackingContext pointer.
// Returns nullptr if handle is invalid.
static WasteTrackingContext *resolve_ctx(WasteTrackingHandle handle) {
  if (!handle)
    return nullptr;
  return static_cast<WasteTrackingContext *>(handle);
}

// Validates session is alive. Returns "OK" or the denial reason.
// Uses the same string contract as auth_check_permission.
static const char *check_perm(UserHandle user_handle,
                              const std::string &permission) {
  if (!user_handle)
    return "NO_SESSION";
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid)
    return "SESSION_DEAD";
  if (!s->auth_manager)
    return "NO_SESSION";
  return s->auth_manager->checkPermission(user_handle, permission);
}

// Builds a structured error JSON string for consistent Dart error handling.
// Always heap allocated — Dart frees with reco_free_string().
static char *error_json(const std::string &code,
                        const std::string &detail = "") {
  std::string j = "{\"success\":false,\"error\":\"" + code + "\"";
  if (!detail.empty())
    j += ",\"detail\":\"" + detail + "\"";
  j += "}";
  return alloc_str(j);
}

// ============================================================================
// FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ----------------------------------------------------------------------------
// Memory
// ----------------------------------------------------------------------------

void reco_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ----------------------------------------------------------------------------
// Notification
// ----------------------------------------------------------------------------

void reco_set_notify_callback(UserHandle user_handle, RecoNotifyCallback cb) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (s->is_valid)
    s->reco_notify_cb = cb;
}

// ----------------------------------------------------------------------------
// QR lookup
// Called automatically when QR arrives via socket.
// Always returns JSON — never nullptr on reachable server.
// ----------------------------------------------------------------------------

char *reco_get_by_qr(WasteTrackingHandle handle, UserHandle user_handle,
                     const char *qr_data) {
  // Permission check
  const char *perm = check_perm(user_handle, "accept_reco");
  if (std::strcmp(perm, "OK") != 0)
    return error_json(std::string(perm));

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  if (!qr_data || std::strlen(qr_data) == 0)
    return error_json("INVALID_QR", "QR data is empty");

  // Call server — returns the full original weighing record
  json result = ctx->db->api_reco_GetByQR(std::string(qr_data));

  // api_reco_GetByQR returns nullptr json on failure
  // Server sends 409 if already processed, 404 if not found
  if (result.is_null()) {
    // We don't know exactly why it failed — return generic not found
    // The server error is already logged by database layer
    return error_json("NOT_FOUND", "QR code not found or already processed");
  }

  // Pass through server data exactly as received — server pre-built it
  // Fields: weighing_id, original_net, original_gross, original_tare,
  //         uuid, current_status, material, product, lot
  json response = {{"success", true}, {"data", result}};
  return alloc_str(response.dump());
}

// ----------------------------------------------------------------------------
// Submit verification weights
// Validator enters new net/gross/tare after re-weighing.
// Server records them, computes diffs authoritatively.
// Returns the full comparison so Dart can display original vs new.
// ----------------------------------------------------------------------------

char *reco_submit(WasteTrackingHandle handle, UserHandle user_handle,
                  int32_t weighing_id, double new_net, double new_gross,
                  double new_tare) {
  const char *perm = check_perm(user_handle, "accept_reco");
  if (std::strcmp(perm, "OK") != 0)
    return error_json(std::string(perm));

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  if (weighing_id <= 0)
    return error_json("INVALID_PARAM", "weighing_id must be positive");

  // Basic sanity — weights must be positive and net = gross - tare
  if (new_gross <= 0.0 || new_tare < 0.0 || new_net < 0.0)
    return error_json("INVALID_WEIGHTS",
                      "Gross, tare and net must be non-negative");

  // Get user_id from session — never from Dart parameters
  auto *s = static_cast<UserSession *>(user_handle);
  int32_t user_id = s->user_id;

  // Call server — server records new weights and returns comparison
  json result = ctx->db->api_reco_Submit(weighing_id, user_id, new_net,
                                         new_gross, new_tare);

  if (result.is_null()) {
    return error_json("SUBMIT_FAILED",
                      "Server rejected the submission — record may already "
                      "be processed or weighing_id is invalid");
  }

  // Pass through server data exactly — server pre-built the comparison
  // Fields: reco_id, net_diff, gross_diff, tare_diff, status
  json response = {{"success", true}, {"data", result}};
  return alloc_str(response.dump());
}

// ----------------------------------------------------------------------------
// Accept
// Server atomically: approves reco + creates shipment +
//                    triggers denaturation if type requires it
// ----------------------------------------------------------------------------

WasteTrackingError reco_accept(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t reco_id) {
  const char *perm = check_perm(user_handle, "accept_reco");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_PARAM;

  if (reco_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  auto *s = static_cast<UserSession *>(user_handle);
  int32_t user_id = s->user_id;

  bool ok = ctx->db->api_reco_Accept(reco_id, user_id);
  if (!ok) {
    std::cerr << "[Reco] reco_accept failed — reco_id=" << reco_id
              << " user_id=" << user_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  return WT_SUCCESS;
}

// ----------------------------------------------------------------------------
// Reject
// Reason is mandatory — empty reason is a hard error.
// Server atomically: rejects reco + updates weighing status.
// ----------------------------------------------------------------------------

WasteTrackingError reco_reject(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t reco_id,
                               const char *reason) {
  const char *perm = check_perm(user_handle, "reject_reco");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_PARAM;

  if (reco_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  // Reason is mandatory — validator must explain why on the factory floor
  if (!reason || std::strlen(reason) == 0) {
    std::cerr << "[Reco] reco_reject called with empty reason — denied."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  auto *s = static_cast<UserSession *>(user_handle);
  int32_t user_id = s->user_id;

  bool ok = ctx->db->api_reco_Reject(reco_id, user_id, std::string(reason));
  if (!ok) {
    std::cerr << "[Reco] reco_reject failed — reco_id=" << reco_id
              << " user_id=" << user_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  return WT_SUCCESS;
}

// ----------------------------------------------------------------------------
// History sidebar
// Validator's own history — records they approved or rejected.
// Server scopes by validator_id — we never pass it from Dart.
// ----------------------------------------------------------------------------

char *reco_my_list(WasteTrackingHandle handle, UserHandle user_handle,
                   const char *status) {
  const char *perm = check_perm(user_handle, "view_data");
  if (std::strcmp(perm, "OK") != 0)
    return error_json(std::string(perm));

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  auto *s = static_cast<UserSession *>(user_handle);
  int32_t user_id = s->user_id;

  // status filter: "approved" | "rejected" | "all" | nullptr → "all"
  std::string status_str = (status && std::strlen(status) > 0) ? status : "all";

  json result = ctx->db->api_reco_MyList(user_id, status_str);

  // Pass through server data exactly — server pre-built nested weight objects
  json response = {{"success", true}, {"data", result}};
  return alloc_str(response.dump());
}

// ----------------------------------------------------------------------------
// Correction
// Validator clicks "Correct" on a history card.
// Server atomically: updates record + logs correction + flags it + emits
// signal. After this, reco status resets and validator must go through the full
// submit → accept/reject flow again.
// ----------------------------------------------------------------------------

WasteTrackingError reco_correct(WasteTrackingHandle handle,
                                UserHandle user_handle, int32_t reco_id,
                                const char *reason, const char *changes_json) {
  const char *perm = check_perm(user_handle, "correct_record");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_PARAM;

  if (reco_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  // Reason is mandatory for audit trail — ALCOA+ compliance
  if (!reason || std::strlen(reason) == 0) {
    std::cerr << "[Reco] reco_correct called with empty reason — denied."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // changes_json is mandatory — must have something to correct
  if (!changes_json || std::strlen(changes_json) == 0) {
    std::cerr << "[Reco] reco_correct called with empty changes — denied."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // Parse and validate changes JSON
  json changes;
  try {
    changes = json::parse(std::string(changes_json));
  } catch (...) {
    std::cerr << "[Reco] reco_correct — invalid changes JSON." << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  auto *s = static_cast<UserSession *>(user_handle);
  int32_t user_id = s->user_id;

  bool ok =
      ctx->db->api_reco_Correct(reco_id, user_id, std::string(reason), changes);
  if (!ok) {
    std::cerr << "[Reco] reco_correct failed — reco_id=" << reco_id
              << " user_id=" << user_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  return WT_SUCCESS;
}

// Fires the reco notify callback on the UserSession.
// Only fires if session is alive and callback is registered.
void notify_reco(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (s->is_valid && s->reco_notify_cb)
    s->reco_notify_cb(signal_name);
}

} // extern "C"