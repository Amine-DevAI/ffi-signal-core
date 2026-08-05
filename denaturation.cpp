#include "denaturation.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static char *denat_alloc(const std::string &s) {
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

static bool denat_check(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!handle || !user_handle)
    return false;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return false;
  return std::strcmp(ctx->auth->checkPermission(user_handle, "denaturation"),
                     "OK") == 0;
}

static WasteTrackingError permission_error(WasteTrackingHandle handle,
                                           UserHandle user_handle) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return WT_ERROR_INVALID_STATE;
  const char *r = ctx->auth->checkPermission(user_handle, "denaturation");
  if (std::strcmp(r, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(r, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  return WT_ERROR_PERMISSION;
}

static char *error_json(const std::string &code,
                        const std::string &detail = "") {
  std::string j = "{\"success\":false,\"error\":\"" + code + "\"";
  if (!detail.empty())
    j += ",\"detail\":\"" + detail + "\"";
  j += "}";
  return denat_alloc(j);
}

// ============================================================================
// DENATURATION FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// denat_scan_by_qr
// Coordinateur scans QR on the bag → gets batch detail before processing.
// Server looks up the pending denaturation_operation for this weighing.
// Blocks if already completed — prevents double denaturation.
// Always returns JSON — never nullptr on reachable server.
// ---------------------------------------------------------------------------
char *denat_scan_by_qr(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *qr_data) {
  if (!denat_check(handle, user_handle))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  if (!qr_data || std::strlen(qr_data) == 0)
    return error_json("INVALID_PARAM", "QR data is empty");

  json result = ctx->db->api_denat_ScanByQR(std::string(qr_data));

  if (result.is_null()) {
    // Server returns nullptr on 404 (not in denat queue) or
    // 400 (already completed) — database layer already logged the reason
    return error_json(
        "NOT_FOUND", "QR not found in denaturation queue or already completed");
  }

  // Pass through server data exactly —
  // fields: denat_id, material, weight_before, weighing_id
  json response = {{"success", true}, {"data", result}};
  return denat_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// denat_submit
// Coordinateur enters final weights after chemical stabilization.
// Server atomically completes the operation and unblocks shipment dispatch.
// user_id comes from the live session — never from Dart input.
// ---------------------------------------------------------------------------
WasteTrackingError denat_submit(WasteTrackingHandle handle,
                                UserHandle user_handle, int32_t denat_id,
                                double brut_after, double net_after,
                                const char *qr_scanned, const char *note) {
  if (!denat_check(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (denat_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  // Both weights must be positive — zero net after denaturation makes no sense
  if (brut_after <= 0.0 || net_after <= 0.0) {
    std::cerr << "[Denat] denat_submit — weights must be positive. "
              << "brut_after=" << brut_after << " net_after=" << net_after
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // qr_scanned is mandatory — second verification that the right bag processed
  if (!qr_scanned || std::strlen(qr_scanned) == 0) {
    std::cerr << "[Denat] denat_submit — qr_scanned is required." << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // note is optional
  std::string note_str =
      (note && std::strlen(note) > 0) ? std::string(note) : "";

  // user_id from the live session — coordinateur_id in the DB record
  auto *s = static_cast<UserSession *>(user_handle);

  bool ok =
      ctx->db->api_denat_Submit(denat_id, s->user_id, brut_after, net_after,
                                std::string(qr_scanned), note_str);

  if (!ok) {
    std::cerr << "[Denat] denat_submit failed — denat_id=" << denat_id
              << " — may already be completed." << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Denat] denat_submit OK — denat_id=" << denat_id
            << " net_after=" << net_after << "kg"
            << " by user_id=" << s->user_id << std::endl;

  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// denat_my_list
// Coordinateur's OWN completed denaturation history — not the pending queue.
// Pending queue lives in pending.cpp — this is history only.
// Server scopes by coordinator_id and status='completed'.
// limit=0 uses server default (20 records).
// ---------------------------------------------------------------------------
char *denat_my_list(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t limit) {
  if (!denat_check(handle, user_handle))
    return error_json("PERMISSION_DENIED");

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return error_json("NO_CONTEXT");

  auto *s = static_cast<UserSession *>(user_handle);

  // Use server default if limit not specified
  int effective_limit = (limit > 0) ? limit : 20;

  json list = ctx->db->api_denat_MyList(s->user_id, effective_limit);

  if (!list.is_array()) {
    std::cerr << "[Denat] denat_my_list — unexpected response from server."
              << std::endl;
    return error_json("SERVER_ERROR");
  }

  json response = {{"success", true}, {"data", list}};
  return denat_alloc(response.dump());
}

// ---------------------------------------------------------------------------
// denat_correct
// Coordinateur fixes wrong weights on a completed operation.
// Server updates weights + appends correction note + logs audit trail.
// reason is mandatory — ALCOA+ compliance requires it.
// ---------------------------------------------------------------------------
WasteTrackingError denat_correct(WasteTrackingHandle handle,
                                 UserHandle user_handle, int32_t denat_id,
                                 double new_net, double new_brut,
                                 const char *reason) {
  if (!denat_check(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (denat_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  // Both corrected weights must be positive
  if (new_net <= 0.0 || new_brut <= 0.0) {
    std::cerr << "[Denat] denat_correct — weights must be positive."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // Reason is mandatory — every correction needs an audit trail
  if (!reason || std::strlen(reason) == 0) {
    std::cerr << "[Denat] denat_correct — reason is required." << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // user_id from the live session — logged as the person making the correction
  auto *s = static_cast<UserSession *>(user_handle);

  bool ok = ctx->db->api_denat_Correct(denat_id, s->user_id, new_net, new_brut,
                                       std::string(reason));

  if (!ok) {
    std::cerr << "[Denat] denat_correct failed — denat_id=" << denat_id
              << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Denat] denat_correct OK — denat_id=" << denat_id
            << " new_net=" << new_net << " new_brut=" << new_brut
            << " by user_id=" << s->user_id << std::endl;

  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// MEMORY
// ---------------------------------------------------------------------------

void denat_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ---------------------------------------------------------------------------
// NOTIFICATION
// ---------------------------------------------------------------------------

void denat_set_notify_callback(UserHandle user_handle, DenatNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Denat] denat_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->denat_notify_cb = cb;
  std::cout << "[Denat] Notify callback registered for user " << s->user_id
            << std::endl;
}

void notify_denat(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Denat] notify_denat — session dead, dropping: "
              << signal_name << std::endl;
    return;
  }

  if (!s->denat_notify_cb) {
    std::cerr << "[Denat] notify_denat — no callback registered, dropping: "
              << signal_name << std::endl;
    return;
  }

  std::cout << "[Denat] → Dart: " << signal_name << std::endl;
  s->denat_notify_cb(signal_name);
}

} // extern "C"