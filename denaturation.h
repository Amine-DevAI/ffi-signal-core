#ifndef DENATURATION_H
#define DENATURATION_H

#include "types.h"
#include <cstdint>

// ============================================================================
// DENATURATION CHAMBER
//
// Handles chemical stabilization of pharmaceutical waste before shipment.
// Only applies to material types where requires_denaturation = TRUE.
// The shipment dispatch is BLOCKED until denaturation is completed.
//
// Flow:
//   Validator accepts reco → server creates denaturation_operation (pending)
//   Server emits DENATURATION_PENDING → coordinateur gets notified
//   Coordinateur scans QR on the bag → denat_scan_by_qr() → sees batch detail
//   Coordinateur performs physical denaturation (adds chemical agent)
//   Coordinateur enters final brut and net weights → denat_submit()
//   Server marks denaturation completed → unblocks shipment dispatch
//   Server emits DENATURATION_SUCCESS → coordinateur history refreshes
//   Coordinateur reviews history → denat_my_list() → clicks card → popup
//   Coordinateur spots error → denat_correct()
//
// Permission model:
//   denat_scan_by_qr → denaturation (COORDINATEUR, ADMIN)
//   denat_submit     → denaturation (COORDINATEUR, ADMIN)
//   denat_my_list    → denaturation (COORDINATEUR, ADMIN)
//   denat_correct    → denaturation (COORDINATEUR, ADMIN)
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//   denat_free_string(). WasteTrackingError → value type, no free needed.
//
// Signal contract:
//   DENATURATION_PENDING → Dart calls denat_my_list() — new job waiting
//   DENATURATION_SUCCESS → Dart calls denat_my_list() — job completed
//   notify_denat() is called by the socket chamber for both signals.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// denat_scan_by_qr
//
// Coordinateur scans QR on the bag → gets full batch detail.
// Server looks up the denaturation_operation linked to this weighing.
// Blocks if the operation is already completed.
//
// Returns heap-allocated JSON string — always, never nullptr on reachable
// server:
//   {"success":true, "data": {
//     "denat_id", "material", "weight_before", "weighing_id"
//   }}
// Error responses:
//   {"success":false, "error":"NOT_FOUND"}         — QR not in denat queue
//   {"success":false, "error":"ALREADY_COMPLETED"} — already denatured
//   {"success":false, "error":"PERMISSION_DENIED"}
//
// Dart MUST free with denat_free_string().
// Permission: denaturation (COORDINATEUR, ADMIN)
// ---------------------------------------------------------------------------
char *denat_scan_by_qr(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *qr_data);

// ---------------------------------------------------------------------------
// denat_submit
//
// Coordinateur enters final weights after chemical stabilization.
// Server atomically:
//   1. Records brut_after and net_after on denaturation_operations
//   2. Sets denaturation status = 'completed'
//   3. Updates weighing status = 'ready_for_shipment'
//   4. Unblocks the shipment dispatch for this batch
//   5. Emits DENATURATION_SUCCESS signal
//
// qr_scanned: the QR code scanned during the denaturation process.
//   Used as a second verification that the right bag was processed.
// note: optional comment. Pass nullptr for default.
//
// Returns:
//   WT_SUCCESS             → denaturation recorded, shipment unblocked
//   WT_ERROR_PERMISSION    → role not allowed
//   WT_ERROR_INVALID_PARAM → denat_id <= 0, invalid weights
//   WT_ERROR_DATABASE      → already completed or server error
//
// Permission: denaturation (COORDINATEUR, ADMIN)
// ---------------------------------------------------------------------------
WasteTrackingError denat_submit(WasteTrackingHandle handle,
                                UserHandle user_handle, int32_t denat_id,
                                double brut_after, double net_after,
                                const char *qr_scanned, const char *note);

// ---------------------------------------------------------------------------
// denat_my_list
//
// Returns the calling coordinateur's completed denaturation history.
// Server scopes results to session user_id.
// limit: max records to return. Pass 0 for default (20).
//
// Returns heap-allocated JSON array string. Free with denat_free_string().
// Each item: id, qr, material, weight_initial, weight_final,
//            agent_mass, timestamp, note.
//
// Permission: denaturation (COORDINATEUR, ADMIN)
// ---------------------------------------------------------------------------
char *denat_my_list(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t limit);

// ---------------------------------------------------------------------------
// denat_correct
//
// Coordinateur clicks "Correct" on a history card → fixes wrong weights.
// Server atomically:
//   1. Updates weight_net_after and weight_brut_after
//   2. Appends correction note to existing comment
//   3. Logs correction in the corrections table (audit trail)
//
// reason is mandatory — ALCOA+ compliance.
// new_net and new_brut must both be positive.
//
// Returns:
//   WT_SUCCESS             → correction applied, audit logged
//   WT_ERROR_PERMISSION    → role not allowed
//   WT_ERROR_INVALID_PARAM → denat_id <= 0, empty reason, invalid weights
//   WT_ERROR_DATABASE      → server rejected the correction
//
// Permission: denaturation (COORDINATEUR, ADMIN)
// ---------------------------------------------------------------------------
WasteTrackingError denat_correct(WasteTrackingHandle handle,
                                 UserHandle user_handle, int32_t denat_id,
                                 double new_net, double new_brut,
                                 const char *reason);

// ---------------------------------------------------------------------------
// MEMORY
//
// denat_free_string
//   Dart MUST call this on every char* received from denaturation functions.
//   Call it when the data is no longer needed — widget disposed, list
//   refreshed, screen closed. Dart owns the timing.
//   Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void denat_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// denat_set_notify_callback
//   Dart calls this ONCE after login, before the socket connects.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_denat
//   Called by the socket chamber when a denaturation signal arrives.
//   signal_name is one of:
//     "DENATURATION_PENDING" → Dart calls denat_my_list() — new job
//     "DENATURATION_SUCCESS" → Dart calls denat_my_list() — job done
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void denat_set_notify_callback(UserHandle user_handle, DenatNotifyCallback cb);
void notify_denat(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // DENATURATION_H