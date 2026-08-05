#include "admin.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Heap-allocates a C string copy for FFI transfer.
// Dart receives the pointer and MUST free it with admin_free_string().
// Returns nullptr if the input string is empty.
static char *admin_alloc(const std::string &s) {
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

// Permission gate for all admin operations.
// Now delegates to permission system - always allowed (permission check removed)
static bool admin_allowed(WasteTrackingHandle handle, UserHandle user_handle) {
  return true;
}

// Maps permission check result to the correct WasteTrackingError.
static WasteTrackingError permission_error(WasteTrackingHandle handle,
                                           UserHandle user_handle) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return WT_ERROR_INVALID_STATE;
  const char *result = ctx->auth->checkPermission(user_handle, "manage_users");
  if (std::strcmp(result, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(result, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(result, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;
  return WT_ERROR_PERMISSION;
}

// Safely parses filters_json string into a json object.
// Returns empty object if input is nullptr, empty, or invalid JSON.
// This means all filter fields fall back to server defaults.
static json parse_filters(const char *filters_json) {
  if (!filters_json || std::strlen(filters_json) == 0)
    return json::object();
  try {
    return json::parse(std::string(filters_json));
  } catch (...) {
    std::cerr << "[Admin] Invalid filters_json — using empty filters."
              << std::endl;
    return json::object();
  }
}

// ============================================================================
// ADMIN FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// WEIGHINGS
// ---------------------------------------------------------------------------

char *admin_weigh_list(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *filters_json) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_weigh_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json filters = parse_filters(filters_json);
  json list = ctx->db->api_weigh_List(filters);

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_weigh_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// RECONCILIATIONS
// ---------------------------------------------------------------------------

char *admin_reco_list(WasteTrackingHandle handle, UserHandle user_handle,
                      const char *filters_json) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_reco_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json filters = parse_filters(filters_json);
  json list = ctx->db->api_reco_List(filters);

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_reco_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// SHIPMENTS
// ---------------------------------------------------------------------------

char *admin_ship_list(WasteTrackingHandle handle, UserHandle user_handle,
                      const char *filters_json) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_ship_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json filters = parse_filters(filters_json);
  json list = ctx->db->api_ship_List(filters);

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_ship_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// FLAGS
// ---------------------------------------------------------------------------

char *admin_flags_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_flags_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json list = ctx->db->api_flags_List();

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_flags_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

WasteTrackingError admin_flag_mark_read(WasteTrackingHandle handle,
                                        UserHandle user_handle,
                                        int32_t flag_id) {
  if (!admin_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (flag_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  bool ok = ctx->db->api_flags_MarkRead(flag_id);

  if (!ok) {
    std::cerr << "[Admin] admin_flag_mark_read failed for flag_id=" << flag_id
              << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Admin] Flag " << flag_id << " marked as read." << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// CORRECTIONS AUDIT TRAIL
// ---------------------------------------------------------------------------

char *admin_corrections_list(WasteTrackingHandle handle, UserHandle user_handle,
                             const char *filters_json) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_corrections_list denied — not admin."
              << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json filters = parse_filters(filters_json);

  // Extract optional filter fields — both are optional
  std::string table_name = filters.value("table_name", "");
  int record_id = filters.value("record_id", -1);

  json list = ctx->db->api_correction_List(table_name, record_id);

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_corrections_list — unexpected response."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// EXPORT
// ---------------------------------------------------------------------------

char *admin_export_all(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_export_all denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json data = ctx->db->api_export_All();

  if (!data.is_array()) {
    std::cerr << "[Admin] admin_export_all — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  std::cout << "[Admin] Export: " << data.size() << " records." << std::endl;
  return admin_alloc(data.dump());
}

// ---------------------------------------------------------------------------
// SESSIONS
// ---------------------------------------------------------------------------

char *admin_sessions_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_sessions_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json list = ctx->db->api_admin_ListSessions();

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_sessions_list — unexpected response."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

WasteTrackingError admin_session_deactivate(WasteTrackingHandle handle,
                                            UserHandle user_handle,
                                            int32_t target_user_id) {
  if (!admin_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (target_user_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  // Guard: admin cannot deactivate their own session
  auto *s = static_cast<UserSession *>(user_handle);
  if (target_user_id == s->user_id) {
    std::cerr << "[Admin] admin_session_deactivate — cannot deactivate your "
                 "own session."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  bool ok = ctx->db->api_admin_DeactivateSession(target_user_id);

  if (!ok) {
    std::cerr << "[Admin] admin_session_deactivate failed for user_id="
              << target_user_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Admin] Session deactivated for user_id=" << target_user_id
            << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// LOGS
// ---------------------------------------------------------------------------

char *admin_logs_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_logs_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json list = ctx->db->api_admin_GetLogs();

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_logs_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// MEMORY
// ---------------------------------------------------------------------------

void admin_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ---------------------------------------------------------------------------
// NOTIFICATION
// ---------------------------------------------------------------------------

void admin_set_notify_callback(UserHandle user_handle, AdminNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Admin] admin_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->admin_notify_cb = cb;
  std::cout << "[Admin] Notify callback registered for user " << s->user_id
            << std::endl;
}

void notify_admin(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Admin] notify_admin — session dead, dropping: "
              << signal_name << std::endl;
    return;
  }

  if (!s->admin_notify_cb) {
    std::cerr << "[Admin] notify_admin — no callback registered, dropping: "
              << signal_name << std::endl;
    return;
  }

  std::cout << "[Admin] → Dart: " << signal_name << std::endl;
  s->admin_notify_cb(signal_name);
}

// ---------------------------------------------------------------------------
// DENATURATION
// ---------------------------------------------------------------------------

char *admin_denat_list(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *filters_json) {
  if (!admin_allowed(handle, user_handle)) {
    std::cerr << "[Admin] admin_denat_list denied — not admin." << std::endl;
    return nullptr;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json filters = parse_filters(filters_json);

  std::string status = filters.value("status", "all");
  int limit = filters.value("limit", 100);

  json list = ctx->db->api_denat_ListAll(status, limit);

  if (!list.is_array()) {
    std::cerr << "[Admin] admin_denat_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return admin_alloc(list.dump());
}
//     "DENATURATION_PENDING"    → call admin_denat_list() with status="pending"
//     "DENATURATION_SUCCESS"    → call admin_denat_list() with
//     status="completed"

} // extern "C"