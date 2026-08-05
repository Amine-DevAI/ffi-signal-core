#ifndef WEIGHING_H
#define WEIGHING_H

#include "types.h"
#include <cstdint>

// ============================================================================
// WEIGHING FFI
//
// The entry point of the entire pharma waste workflow.
// Every gram of waste tracked in this system starts here.
//
// Flow:
//   Dart reads gross and tare from the scale (independent serial port layer)
//   Dart calls weighing_create() with confirmed numbers
//   C++ computes net = gross - tare, generates UUID, sends to server
//   Server atomically creates the weighing + reconciliation placeholder
//   weighing_create() returns the UUID string → Dart prints the QR label
//   From this point the UUID is the permanent physical link between
//   the DB record and the physical bag/box on the factory floor
//
// Permission model:
//   weighing_create   → OPERATOR, ADMIN   ("create_weighing")
//   weighing_correct  → OPERATOR, ADMIN,
//                       VALIDATOR, COORDINATEUR ("correct_record")
//   weighing_get_by_uuid → any valid session ("view_data")
//   weighing_my_list  → any valid session ("view_data")
//                       server scopes results to operator's own records
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//                        weighing_free_string().
//   WasteTrackingError → value type, no free needed.
//
// Signal contract:
//   Weighing chamber owns one signal: WEIGHING_CORRECTED
//   Socket chamber calls notify_weighing(user_handle, "WEIGHING_CORRECTED")
//   Dart receives it and calls weighing_my_list() to refresh the operator view.
//   NEW_FLAG_ALERT is routed to admin chamber — operators don't see flags.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// weighing_create
//
// Creates a weighing record and its reconciliation placeholder atomically.
// Net weight is computed server-side from gross and tare.
// QR code is generated server-side as W-YYMMDD-XXXX (database sovereign).
//
// Parameters:
//   type_id        → required. Must match a valid id in the types table.
//   product_id     → pass 0 if type does not require_product.
//   gross_weight   → total scale reading including container (Poids Brut).
//   tare_weight    → container weight (Poids Tare).
//   lot_number     → pass nullptr or "" if type does not require_lot.
//
// Returns:
//   QR code string on success — Dart uses this immediately to print the label.
//   Format: W-YYMMDD-XXXX (e.g., W260410-1025).
//   nullptr on any failure: permission denied, missing lot number for a type
//   that requires it, DB error, or server unreachable.
//
// The QR code is the permanent physical identifier for this waste batch.
// Dart should store it locally for the session — it cannot be recovered
// from this function a second time.
// ---------------------------------------------------------------------------
char *weighing_create(WasteTrackingHandle handle, UserHandle user_handle,
                      int32_t type_id, int32_t product_id, double gross_weight,
                      double tare_weight, const char *lot_number);

// ---------------------------------------------------------------------------
// weighing_get_by_uuid
//
// Fetches the full detail record for one weighing by its QR code.
// Used when the operator or validator scans a QR code on the floor.
//
// Returns:
//   Heap-allocated JSON object string on success. Free with
//   weighing_free_string(). Contains: internal_id, weight_net, weight_gross,
//   weight_tare, lot, status, timestamp, material, product, operator,
//   qr_code, is_flagged.
//   nullptr if qr_code is not found or session is invalid.
// ---------------------------------------------------------------------------
char *weighing_get_by_uuid(WasteTrackingHandle handle, UserHandle user_handle,
                           const char *uuid);

// ---------------------------------------------------------------------------
// weighing_my_list
//
// Returns the calling operator's last 50 weighings, newest first.
// Server scopes results to the session user_id — operators never see
// each other's records through this endpoint.
//
// Returns:
//   Heap-allocated JSON array string. Free with weighing_free_string().
//   Each item: id, material, product, weight_net, weight_gross, weight_tare,
//   lot, status, created_at, uuid, is_flagged.
//   nullptr if session is invalid or server is unreachable.
// ---------------------------------------------------------------------------
char *weighing_my_list(WasteTrackingHandle handle, UserHandle user_handle);

// ---------------------------------------------------------------------------
// weighing_correct
//
// Submits a correction to an existing weighing record.
// Server atomically:
//   1. Updates the weighing fields provided in changes_json
//   2. Recalculates net weight from updated gross and tare
//   3. Logs the correction in the corrections table (audit trail)
//   4. Raises a flag in the flags table (admin alert)
//   5. Sets is_flagged = TRUE on the weighing record
//
// changes_json: partial JSON with only the fields being corrected.
//   Allowed keys: type_id, product_id, lot_number, gross_weight, tare_weight
//   Example: "{\"gross_weight\":12.5,\"tare_weight\":0.8}"
//   Unknown keys are silently ignored by the server.
//
// reason: mandatory human-readable explanation for the audit log.
//   Cannot be empty — server rejects the request without it.
//
// Returns:
//   WT_SUCCESS         → correction applied, flag raised, audit logged
//   WT_ERROR_PERMISSION → caller role cannot correct records
//   WT_ERROR_INVALID_PARAM → weighing_id <= 0, empty reason, invalid JSON
//   WT_ERROR_DATABASE  → server rejected the correction
// ---------------------------------------------------------------------------
WasteTrackingError weighing_correct(WasteTrackingHandle handle,
                                    UserHandle user_handle, int32_t weighing_id,
                                    const char *reason,
                                    const char *changes_json);

// ---------------------------------------------------------------------------
// MEMORY
//
// weighing_free_string
//   Dart MUST call this on every char* received from weighing functions.
//   Call it when the data is no longer needed — after QR label is printed,
//   widget disposed, list refreshed, screen closed.
//   Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void weighing_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// weighing_set_notify_callback
//   Dart calls this ONCE after login, before the socket connects.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_weighing
//   Called by the socket chamber when WEIGHING_CORRECTED arrives.
//   signal_name is always "WEIGHING_CORRECTED".
//   Dart receives it and calls weighing_my_list() to refresh the view.
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void weighing_set_notify_callback(UserHandle user_handle,
                                  WeighingNotifyCallback cb);

void notify_weighing(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // WEIGHING_H