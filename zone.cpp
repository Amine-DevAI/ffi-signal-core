#include "zone.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Heap-allocates a C string copy for FFI transfer.
// Dart receives the pointer and MUST free it with zone_free_string().
// Returns nullptr if the input string is empty.
static char *zone_alloc(const std::string &s) {
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

// Permission gate for write operations.
// Returns true only if session is alive AND role is ADMIN.
static bool zone_write_allowed(WasteTrackingHandle handle,
                               UserHandle user_handle) {
  if (!handle || !user_handle)
    return false;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return false;
  const char *result = ctx->auth->checkPermission(user_handle, "manage_users");
  return (std::strcmp(result, "OK") == 0);
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

// ============================================================================
// ZONE FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// zone_list
// Open read — no user_handle needed.
// Server only returns active zones (WHERE active = TRUE).
// Returns heap-allocated JSON array string. Dart frees with zone_free_string().
// Returns nullptr if context is invalid or server is unreachable.
// ---------------------------------------------------------------------------
char *zone_list(WasteTrackingHandle handle) {
  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json list = ctx->db->api_zone_List();

  if (!list.is_array()) {
    std::cerr << "[Zone] zone_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return zone_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// zone_insert
// Admin only. code must be unique — server enforces it.
// Server defaults active=true on creation.
// Returns 1 on success, -1 on failure.
// ---------------------------------------------------------------------------
int32_t zone_insert(WasteTrackingHandle handle, UserHandle user_handle,
                    const char *code, const char *name) {
  if (!zone_write_allowed(handle, user_handle)) {
    std::cerr << "[Zone] zone_insert denied — permission check failed."
              << std::endl;
    return -1;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return -1;

  if (!code || std::strlen(code) == 0) {
    std::cerr << "[Zone] zone_insert — code is required." << std::endl;
    return -1;
  }
  if (!name || std::strlen(name) == 0) {
    std::cerr << "[Zone] zone_insert — name is required." << std::endl;
    return -1;
  }

  json result = ctx->db->api_zone_Create(std::string(code), std::string(name));

  if (!result.value("success", false)) {
    std::cerr << "[Zone] zone_insert failed — code='" << code
              << "' may already exist: "
              << result.value("error_message", "unknown") << std::endl;
    return -1;
  }

  std::cout << "[Zone] zone_insert OK — code='" << code << "'" << std::endl;
  return 1;
}

// ---------------------------------------------------------------------------
// zone_update
// Admin only. Handles both rename and soft-delete in one function.
// Pass active=false to soft-delete — zone disappears from zone_list().
// Pass active=true to restore a previously soft-deleted zone.
// ---------------------------------------------------------------------------
WasteTrackingError zone_update(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t zone_id,
                               const char *code, const char *name,
                               bool active) {
  if (!zone_write_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (zone_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  if (!code || std::strlen(code) == 0)
    return WT_ERROR_INVALID_PARAM;

  if (!name || std::strlen(name) == 0)
    return WT_ERROR_INVALID_PARAM;

  json result = ctx->db->api_zone_Update(zone_id, std::string(code),
                                         std::string(name), active);

  if (!result.value("success", false)) {
    std::cerr << "[Zone] zone_update failed for id=" << zone_id << " — "
              << result.value("error_message", "unknown") << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Zone] zone_update OK — id=" << zone_id << " code='" << code
            << "' active=" << active << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// zone_free_string
// Dart calls this on every char* received from zone functions.
// Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void zone_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ---------------------------------------------------------------------------
// zone_set_notify_callback
// Dart calls this once after login, before the socket connects.
// Stores callback on UserSession — survives socket rebuilds.
// Dart MUST use NativeCallable.permanent() to prevent GC collection.
// ---------------------------------------------------------------------------
void zone_set_notify_callback(UserHandle user_handle, ZoneNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Zone] zone_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->zone_notify_cb = cb;
  std::cout << "[Zone] Notify callback registered for user " << s->user_id
            << std::endl;
}

// ---------------------------------------------------------------------------
// notify_zone
// Called by the socket chamber when REFRESH_ZONES arrives.
// Fires Dart's callback with the signal name as a static string literal.
// Dart receives "REFRESH_ZONES" and calls zone_list().
// Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void notify_zone(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Zone] notify_zone — session dead, dropping: " << signal_name
              << std::endl;
    return;
  }

  if (!s->zone_notify_cb) {
    std::cerr << "[Zone] notify_zone — no callback registered, dropping: "
              << signal_name << std::endl;
    return;
  }

  std::cout << "[Zone] → Dart: " << signal_name << std::endl;
  s->zone_notify_cb(signal_name);
}

} // extern "C"