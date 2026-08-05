#ifndef ADMIN_H
#define ADMIN_H

#include "types.h"
#include <cstdint>

// ============================================================================
// ADMIN FFI
//
// God-view chamber. Only USER_ROLE_ADMIN can call any function here.
// Every function gates on checkPermission("manage_users") — returns
// nullptr or WT_ERROR_PERMISSION if the caller is not admin.
//
// What admin owns:
//   - Full weighing list     (all operators, all statuses)
//   - Full reco list         (all validators, all statuses)
//   - Full shipment list     (all shippers, all statuses)
//   - Flags list + mark read (audit alerts)
//   - Corrections audit trail
//   - Full data export
//   - Active sessions list + force-deactivate
//   - Login attempt logs
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//   admin_free_string(). WasteTrackingError → value type, no free needed.
//
// Filter contract:
//   List functions accept filters_json — a JSON object string Dart builds.
//   Pass nullptr or "{}" for no filter (returns everything up to default
//   limit). Common filter keys per domain are documented below. Unknown keys
//   are silently ignored by the server.
//
// Signal contract:
//   Server emits signals after workflow events.
//   Socket chamber calls notify_admin(user_handle, signal_name).
//   Dart matches signal_name and calls the appropriate list function.
//   Signals admin receives:
//     "NEW_FLAG_ALERT"      → call admin_flags_list()
//     "FLAG_RESOLVED"       → call admin_flags_list()
//     "WEIGHING_CORRECTED"  → call admin_weigh_list()
//     "RECO_STATE_UPDATED"  → call admin_reco_list()
//     "SHIPMENT_DISPATCHED" → call admin_ship_list()
//     "USER_CREATED"        → call user_list()    (auth chamber handles this)
//     "USER_UPDATED"        → call user_list()    (auth chamber handles this)
//     "USER_DEACTIVATED"    → call user_list()    (auth chamber handles this)
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// WEIGHINGS
//
// admin_weigh_list
//   Returns full JSON array of weighings across all operators.
//   filters_json keys: status, type_id, search, limit, offset
//   Example: "{\"status\":\"pending\",\"limit\":50}"
//   Pass nullptr or "{}" for unfiltered list (default limit 100).
// ---------------------------------------------------------------------------
char *admin_weigh_list(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *filters_json);

// ---------------------------------------------------------------------------
// RECONCILIATIONS
//
// admin_reco_list
//   Returns full JSON array of reconciliations across all validators.
//   filters_json keys: status, search, limit, offset
//   Example: "{\"status\":\"scanned\",\"limit\":50}"
//   Pass nullptr or "{}" for unfiltered list (default limit 50).
// ---------------------------------------------------------------------------
char *admin_reco_list(WasteTrackingHandle handle, UserHandle user_handle,
                      const char *filters_json);

// ---------------------------------------------------------------------------
// SHIPMENTS
//
// admin_ship_list
//   Returns full JSON array of shipments across all shippers.
//   filters_json keys: filter, start, limit
//     filter: "all" | "pending" | "shipped" | "flagged"
//     start:  date string "YYYY-MM-DD" for date filtering
//   Example: "{\"filter\":\"shipped\",\"limit\":50}"
//   Pass nullptr or "{}" for unfiltered list (default limit 50).
// ---------------------------------------------------------------------------
char *admin_ship_list(WasteTrackingHandle handle, UserHandle user_handle,
                      const char *filters_json);

// ---------------------------------------------------------------------------
// FLAGS
//
// admin_flags_list
//   Returns full JSON array of unresolved flags.
//   No filters — admin sees all active flags.
//   Each item includes: id, table, record_id, reason, user_name, hint,
//   created_at, is_resolved.
//
// admin_flag_mark_read
//   Marks a flag as resolved. Server emits FLAG_RESOLVED signal.
//   Returns WT_SUCCESS or WT_ERROR_DATABASE.
// ---------------------------------------------------------------------------
char *admin_flags_list(WasteTrackingHandle handle, UserHandle user_handle);

WasteTrackingError admin_flag_mark_read(WasteTrackingHandle handle,
                                        UserHandle user_handle,
                                        int32_t flag_id);

// ---------------------------------------------------------------------------
// CORRECTIONS AUDIT TRAIL
//
// admin_corrections_list
//   Returns full JSON array of correction records.
//   filters_json keys: table_name, record_id
//     table_name: "weighings" | "reconciliations" | "shipments" | ""
//     record_id:  specific record to audit, or omit for all
//   Example: "{\"table_name\":\"weighings\"}"
//   Pass nullptr or "{}" for the full unfiltered audit trail (limit 200).
// ---------------------------------------------------------------------------
char *admin_corrections_list(WasteTrackingHandle handle, UserHandle user_handle,
                             const char *filters_json);

// ---------------------------------------------------------------------------
// EXPORT
//
// admin_export_all
//   Returns the full traceability master view as a JSON array.
//   No filters — this is the complete audit dump.
//   Can be large — Dart should call this only when explicitly requested
//   (e.g. export button), not on every screen load.
//   Each item contains the full workflow chain per weighing:
//   operator, material, weights, reco status, shipment status,
//   denaturation status, validator, approval date.
// ---------------------------------------------------------------------------
char *admin_export_all(WasteTrackingHandle handle, UserHandle user_handle);

// ---------------------------------------------------------------------------
// SESSIONS
//
// admin_sessions_list
//   Returns JSON array of currently active sessions (last 5 minutes).
//   Each item: user_id, username, ip, last_active, version.
//
// admin_session_deactivate
//   Force-kills all active sessions for target_user_id.
//   Server also sends FORCE_LOGOUT via heartbeat to that user instantly.
//   Returns WT_SUCCESS or WT_ERROR_DATABASE.
// ---------------------------------------------------------------------------
char *admin_sessions_list(WasteTrackingHandle handle, UserHandle user_handle);

WasteTrackingError admin_session_deactivate(WasteTrackingHandle handle,
                                            UserHandle user_handle,
                                            int32_t target_user_id);

// ---------------------------------------------------------------------------
// LOGS
//
// admin_logs_list
//   Returns JSON array of last 100 login attempts.
//   Each item: username, success, ip, time, error.
//   Useful for security auditing — who tried to log in and failed.
// ---------------------------------------------------------------------------
char *admin_logs_list(WasteTrackingHandle handle, UserHandle user_handle);

// ---------------------------------------------------------------------------
// DENATURATION
//
// admin_denat_list
//   Returns full JSON array of all denaturation operations.
//   filters_json keys: status, limit
//     status: "pending" | "completed" | "all"
//     limit:  max records to return (default 100)
//   Example: "{\"status\":\"pending\",\"limit\":50}"
//   Pass nullptr or "{}" for all records, all statuses.
//   Each item contains: id, qr, material, weights (before/after/delta),
//   status, operator name, created_at, completed_at, note.
// ---------------------------------------------------------------------------
char *admin_denat_list(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *filters_json);
// ---------------------------------------------------------------------------
// MEMORY
//
// admin_free_string
//   Dart MUST call this on every char* received from admin functions.
//   Call it when the data is no longer needed — widget disposed, list
//   refreshed, screen closed. Dart owns the timing.
//   Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void admin_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// admin_set_notify_callback
//   Dart calls this ONCE after login, before the socket connects.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_admin
//   Called by the socket chamber when an admin-relevant signal arrives.
//   signal_name is one of:
//     "NEW_FLAG_ALERT"      → Dart calls admin_flags_list()
//     "FLAG_RESOLVED"       → Dart calls admin_flags_list()
//     "WEIGHING_CORRECTED"  → Dart calls admin_weigh_list()
//     "RECO_STATE_UPDATED"  → Dart calls admin_reco_list()
//     "SHIPMENT_DISPATCHED" → Dart calls admin_ship_list()
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void admin_set_notify_callback(UserHandle user_handle, AdminNotifyCallback cb);

void notify_admin(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // ADMIN_H