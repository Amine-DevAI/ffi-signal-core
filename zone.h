#ifndef ZONE_H
#define ZONE_H

#include "types.h"
#include <cstdint>

// ============================================================================
// ZONE FFI
//
// Storage zones are reference data — same pattern as catalog.
// Mutations are admin-only.
// Reads are open to any valid context — operators need zones when weighing.
//
// Permission model:
//   Reads  → context handle only. No user_handle needed.
//   Writes → admin only. Enforced by checkPermission("manage_users").
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//   zone_free_string(). int32_t (insert)   → 1 on success, -1 on failure.
//   WasteTrackingError → value type, no free needed.
//
// Signal contract:
//   Server emits REFRESH_ZONES after any zone mutation.
//   Socket chamber calls notify_zone(user_handle, "REFRESH_ZONES").
//   Dart receives it and calls zone_list() to get fresh data.
//   The callback lives on UserSession — survives socket rebuilds.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// READS — no user_handle needed
//
// zone_list
//   Returns full JSON array of active zones, ordered by code ASC.
//   Server filters inactive (soft-deleted) zones automatically.
//   Returns nullptr if context is invalid or server is unreachable.
//   Dart frees with zone_free_string().
// ---------------------------------------------------------------------------
char *zone_list(WasteTrackingHandle handle);

// ---------------------------------------------------------------------------
// WRITES — admin only
//
// zone_insert
//   Creates a new zone. code must be unique (server enforces it).
//   Returns 1 on success, -1 on failure.
//   Server defaults active=true — no need to pass it on creation.
//
// zone_update
//   Updates code and name for an existing zone.
//   Pass active=false to soft-delete — server hides it from zone_list().
//   Pass active=true to restore a previously soft-deleted zone.
//
// No hard delete — zones are soft-deleted via zone_update(active=false).
// This preserves the audit trail for weighings that reference the zone.
// ---------------------------------------------------------------------------
int32_t zone_insert(WasteTrackingHandle handle, UserHandle user_handle,
                    const char *code, const char *name);

WasteTrackingError zone_update(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t zone_id,
                               const char *code, const char *name, bool active);

// ---------------------------------------------------------------------------
// MEMORY
//
// zone_free_string
//   Dart MUST call this on every char* received from zone functions.
//   Call it when the data is no longer needed — widget disposed, list
//   refreshed, screen closed. Dart owns the timing.
//   Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void zone_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// zone_set_notify_callback
//   Dart calls this ONCE after login, before the socket connects.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_zone
//   Called by the socket chamber when REFRESH_ZONES arrives.
//   signal_name is always "REFRESH_ZONES" → Dart calls zone_list().
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void zone_set_notify_callback(UserHandle user_handle, ZoneNotifyCallback cb);
void notify_zone(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // ZONE_H