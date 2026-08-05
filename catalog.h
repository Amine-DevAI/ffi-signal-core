#ifndef CATALOG_H
#define CATALOG_H

#include "types.h"
#include <cstdint>

// ============================================================================
// CATALOG FFI
//
// Manages two static reference datasets:
//   - Material types  (types table: T-01 … T-21)
//   - Brand products
//
// Permission model:
//   Reads  → context handle only. Any valid context can read.
//             No user_handle required — these are reference lookups.
//   Writes → admin only. Enforced by checkPermission("manage_users").
//             Passing a non-admin user_handle returns WT_ERROR_PERMISSION.
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//                        context_free_string().
//   int32_t (create)   → new record id on success, -1 on failure.
//   WasteTrackingError → value type, no free needed.
//
// Signal contract:
//   After any successful write the server emits a WebSocket signal:
//     TYPE_CREATED / TYPE_UPDATED / TYPE_DELETED → Dart calls type_list()
//     REFRESH_PRODUCTS                           → Dart calls product_list()
//
//   Flow:
//     Socket receives signal
//     → calls notify_catalog(user_handle, signal_name)
//     → notify_catalog fires CatalogNotifyCallback registered by Dart
//     → Dart matches signal_name and calls the right endpoint
//     → Dart gets fresh data, displays it, frees pointer when widget disposes
//
//   The callback lives on UserSession — survives socket rebuilds.
//   No client-side cache exists — every list call hits the server fresh.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// MATERIAL TYPES
//
// type_create  — all fields required at creation time.
//   requires_product     → server enforces product selection on weighing
//   requires_lot         → server enforces lot number on weighing
//   requires_name        → server enforces name field on weighing
//   requires_denaturation→ triggers denaturation workflow after approval
//
// type_update  — partial update via fields_json.
//   fields_json must be a valid JSON object containing ONLY the fields
//   you want to change, plus "id". Example:
//     "{\"name\":\"New Name\",\"requires_denaturation\":true,\"id\":5}"
//   Allowed keys: code, name, nature, forme, tag_code,
//                 requires_lot, requires_name, requires_product,
//                 requires_denaturation
//   Unknown keys are silently ignored by the server whitelist.
//
// type_delete  — hard delete. Server blocks with WT_ERROR_DATABASE if
//   the type has existing weighings (foreign key constraint).
//   Update the name instead of deleting to preserve audit trail.
//
// type_list    — returns full JSON array of all types, optionally filtered.
//   search_term matches against code, name, or nature (case-insensitive).
//   Pass nullptr or empty string for the full unfiltered list.
//
// type_get     — returns a single type object by id.
//   Returns nullptr if not found.
// ---------------------------------------------------------------------------

int32_t type_create(WasteTrackingHandle handle, UserHandle user_handle,
                    const char *code, const char *name, const char *nature,
                    const char *forme, int32_t tag_code, bool requires_lot,
                    bool requires_name, bool requires_product,
                    bool requires_denaturation);

WasteTrackingError type_update(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t type_id,
                               const char *fields_json);

WasteTrackingError type_delete(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t type_id);

// Read — no user_handle needed
char *type_list(WasteTrackingHandle handle, const char *search_term);
char *type_get(WasteTrackingHandle handle, int32_t type_id);

// ---------------------------------------------------------------------------
// BRAND PRODUCTS
//
// product_create — name must be unique (server enforces ON CONFLICT).
//   Returns new id on success, -1 on failure (e.g. duplicate name).
//
// product_update — replaces the name for the given product_id.
//
// product_delete — hard delete. Safe only if no weighings reference
//   this product. Server returns an error if foreign key blocks it.
//
// product_list   — returns full JSON array, ordered by name ASC.
//   No search filter — product list is short enough to filter client-side.
// ---------------------------------------------------------------------------

int32_t product_create(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *name);

WasteTrackingError product_update(WasteTrackingHandle handle,
                                  UserHandle user_handle, int32_t product_id,
                                  const char *name);

WasteTrackingError product_delete(WasteTrackingHandle handle,
                                  UserHandle user_handle, int32_t product_id);

// Read — no user_handle needed
char *product_list(WasteTrackingHandle handle);

// ---------------------------------------------------------------------------
// MEMORY
//
// catalog_free_string
//   Dart MUST call this on every char* received from catalog functions:
//     type_list(), type_get(), product_list()
//   Call it when the data is no longer needed — widget disposed, list
//   refreshed, screen closed, whatever. Dart owns the timing.
//   Safe to call with nullptr — does nothing.
//   Never call free() directly — always go through this function.
// ---------------------------------------------------------------------------
void catalog_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// catalog_set_notify_callback
//   Dart calls this ONCE after login to register its listener.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_catalog
//   Called by the socket chamber when a catalog signal arrives.
//   signal_name is one of:
//     "TYPE_CREATED"     → Dart calls type_list()
//     "TYPE_UPDATED"     → Dart calls type_list()
//     "TYPE_DELETED"     → Dart calls type_list()
//     "REFRESH_PRODUCTS" → Dart calls product_list()
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread — checks is_valid before firing.
// ---------------------------------------------------------------------------
void catalog_set_notify_callback(UserHandle user_handle,
                                 CatalogNotifyCallback cb);

void notify_catalog(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // CATALOG_H