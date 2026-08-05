#ifndef SHIPMENT_H
#define SHIPMENT_H

#include "types.h"
#include <cstdint>

// ============================================================================
// SHIPMENT CHAMBER
//
// Handles the physical dispatch of approved waste batches onto the truck.
// Every shipment starts as a reconciliation that was accepted by a validator.
//
// Flow:
//   Validator accepts reco → server creates shipment with status='pending'
//   Server emits SHIPMENT_READY → shipper's pending queue updates
//   Shipper opens app → sees pending queue (pending.cpp handles that view)
//   Shipper scans QR on the bag → ship_get_by_qr() → sees full batch detail
//   Shipper confirms dispatch → ship_dispatch() → server atomic triple-sync:
//     shipments.status     → 'shipped'
//     reconciliations.status → 'shipped'
//     weighings.status     → 'shipped'
//     GUARD: blocks if type requires_denaturation and denat is not completed
//   Server emits SHIPMENT_DISPATCHED → history refreshes
//   Shipper reviews history → ship_my_list() → clicks card → popup
//   Shipper spots error → ship_correct_status() → status corrected + audited
//
// Permission model:
//   ship_get_by_qr      → dispatch_shipment (VALIDATOR, ADMIN)
//   ship_dispatch       → dispatch_shipment (VALIDATOR, ADMIN)
//   ship_my_list        → view_data (any valid session)
//   ship_correct_status → dispatch_shipment (VALIDATOR, ADMIN)
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//   ship_free_string(). WasteTrackingError → value type, no free needed.
//
// Signal contract:
//   SHIPMENT_READY      → Dart calls ship_my_list() — new job waiting
//   SHIPMENT_DISPATCHED → Dart calls ship_my_list() — history updated
//   notify_ship() is called by the socket chamber for both signals.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// ship_get_by_qr
//
// Shipper scans QR code on physical bag → gets full batch detail.
// Server returns verified weights (from reconciliation), material,
// product, lot, zone, and current shipment status.
//
// Returns heap-allocated JSON string on success:
//   {"success":true, "data": {
//     "shipment_id", "weights":{"net","gross","tare"},
//     "material", "product", "lot", "zone"
//   }}
// Returns error JSON on failure — never nullptr on reachable server:
//   {"success":false, "error":"NOT_FOUND"}
//   {"success":false, "error":"ALREADY_DISPATCHED"}
//
// Dart MUST free with ship_free_string().
// Permission: dispatch_shipment (VALIDATOR, ADMIN)
// ---------------------------------------------------------------------------
char *ship_get_by_qr(WasteTrackingHandle handle, UserHandle user_handle,
                     const char *qr_data);

// ---------------------------------------------------------------------------
// ship_dispatch
//
// Shipper confirms the batch is loaded onto the truck.
// Server atomically updates shipment + reco + weighing to 'shipped'.
// Denaturation guard: server blocks if type requires_denaturation
// and denaturation is not yet completed — returns DENAT_INCOMPLETE error.
//
// note: optional shipping note. Pass nullptr for default "Loaded onto truck".
//
// Returns heap-allocated JSON string:
//   {"success":true, "data": {
//     "shipment_id", "flagged_warning": bool
//   }}
//   flagged_warning=true means dispatch succeeded but the batch was
//   previously flagged — Dart should show a warning banner.
//
// Error responses (never nullptr on reachable server):
//   {"success":false, "error":"DENAT_INCOMPLETE"}  — denaturation not done
//   {"success":false, "error":"ALREADY_DISPATCHED"} — already shipped
//   {"success":false, "error":"INVALID_PARAM"}
//
// Dart MUST free with ship_free_string().
// Permission: dispatch_shipment (VALIDATOR, ADMIN)
// ---------------------------------------------------------------------------
char *ship_dispatch(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t shipment_id, const char *note);

// ---------------------------------------------------------------------------
// ship_my_list
//
// Returns the calling shipper's dispatch history — shipped and cancelled.
// Server scopes results to session user_id — shippers only see their own.
// status filter: "shipped" | "cancelled" | "all"
// Pass nullptr or "all" for no filter.
//
// Returns heap-allocated JSON array string. Free with ship_free_string().
// Each item contains full shipment detail for popup display:
//   id, status, shipped_at, note, is_flagged, material, product,
//   weight, meta: {was_denatured, denat_status}
//
// Permission: view_data (any valid session)
// ---------------------------------------------------------------------------
char *ship_my_list(WasteTrackingHandle handle, UserHandle user_handle,
                   const char *status);

// ---------------------------------------------------------------------------
// ship_correct_status
//
// Shipper clicks "Correct" on a history card → fixes a wrong status.
// Server atomically updates shipment status + logs correction in audit trail.
// new_status must be one of: "pending", "shipped", "cancelled"
// reason is mandatory — empty reason returns WT_ERROR_INVALID_PARAM.
//
// Returns:
//   WT_SUCCESS             → status corrected, audit logged
//   WT_ERROR_PERMISSION    → role not allowed
//   WT_ERROR_INVALID_PARAM → invalid shipment_id, empty reason,
//                            or invalid status string
//   WT_ERROR_DATABASE      → server rejected the correction
//
// Permission: dispatch_shipment (VALIDATOR, ADMIN)
// ---------------------------------------------------------------------------
WasteTrackingError ship_correct_status(WasteTrackingHandle handle,
                                       UserHandle user_handle,
                                       int32_t shipment_id,
                                       const char *new_status,
                                       const char *reason);

// ---------------------------------------------------------------------------
// MEMORY
//
// ship_free_string
//   Dart MUST call this on every char* received from shipment functions.
//   Call it when the data is no longer needed — widget disposed, list
//   refreshed, screen closed. Dart owns the timing.
//   Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void ship_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// ship_set_notify_callback
//   Dart calls this ONCE after login, before the socket connects.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_ship
//   Called by the socket chamber when a shipment signal arrives.
//   signal_name is one of:
//     "SHIPMENT_READY"      → Dart calls ship_my_list() or pending list
//     "SHIPMENT_DISPATCHED" → Dart calls ship_my_list()
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void ship_set_notify_callback(UserHandle user_handle, ShipNotifyCallback cb);
void notify_ship(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // SHIPMENT_H