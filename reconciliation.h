#ifndef RECONCILIATION_H
#define RECONCILIATION_H

#include "types.h"
#include <cstdint>

// ============================================================================
// RECONCILIATION CHAMBER
//
// Flow:
//   QR arrives via socket → socket layer calls reco_get_by_qr automatically
//   Server returns original weighing record → Dart displays to validator
//   Validator re-weighs → enters new net/gross/tare → calls reco_submit
//   Server records new weights, computes diffs → returns full comparison
//   Dart displays original vs new for each weight
//   Validator decides → reco_accept or reco_reject (reason mandatory)
//   History sidebar via socket signal → Dart calls reco_my_list
//   Validator clicks card → popup → "Correct" button → reco_correct
//   Server flags it, logs correction, emits signal → reco starts from scratch
//
// Pending queue lives in pending.cpp/h — NOT here.
//
// Signals this chamber owns:
//   RECO_STEP_COMPLETED → Dart calls reco_my_list()
//   RECO_REJECTED       → Dart calls reco_my_list()
//   RECO_STATE_UPDATED  → Dart calls reco_my_list()
//
// Memory contract:
//   char*              → heap allocated, Dart frees with reco_free_string()
//   WasteTrackingError → value type, no free needed
//   const char*        → static literal, NEVER free
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Memory
// ----------------------------------------------------------------------------
void reco_free_string(char *ptr);

// ----------------------------------------------------------------------------
// Notification
// Register BEFORE sockets connect.
// Dart uses NativeCallable.permanent() — prevents GC collection.
// signal_name is one of: "RECO_STEP_COMPLETED", "RECO_REJECTED",
//                        "RECO_STATE_UPDATED"
// Dart matches signal_name to the right refresh call:
//   any signal → reco_my_list()
// ----------------------------------------------------------------------------
void reco_set_notify_callback(UserHandle user_handle, RecoNotifyCallback cb);

// ----------------------------------------------------------------------------
// QR lookup
// Called automatically when QR arrives via socket.
// Returns heap-allocated JSON string — always, never nullptr on reachable
// server.
//
// On success:
//   {"success":true, "data": {
//     "weighing_id", "original_net", "original_gross", "original_tare",
//     "uuid", "current_status", "material", "product", "lot"
//   }}
// On 409 (already processed):
//   {"success":false, "error":"ALREADY_PROCESSED", "status":"<status>"}
// On 404 (not found):
//   {"success":false, "error":"NOT_FOUND"}
// On server unreachable:
//   {"success":false, "error":"UNREACHABLE"}
//
// Dart MUST free with reco_free_string()
// Permission: accept_reco (VALIDATOR, ADMIN)
// ----------------------------------------------------------------------------
char *reco_get_by_qr(WasteTrackingHandle handle, UserHandle user_handle,
                     const char *qr_data);

// ----------------------------------------------------------------------------
// Submit verification weights
// Validator enters new net/gross/tare after re-weighing.
// Server records them authoritatively and returns the full comparison.
//
// Returns heap-allocated JSON string on success:
//   {"reco_id", "net_diff", "gross_diff", "tare_diff", "status"}
// Returns error JSON on failure — never nullptr on reachable server.
//
// Dart MUST free with reco_free_string()
// Permission: accept_reco (VALIDATOR, ADMIN)
// ----------------------------------------------------------------------------
char *reco_submit(WasteTrackingHandle handle, UserHandle user_handle,
                  int32_t weighing_id, double new_net, double new_gross,
                  double new_tare);

// ----------------------------------------------------------------------------
// Validator decisions
// ----------------------------------------------------------------------------

// Accept — server atomically creates shipment + triggers denaturation if needed
// Returns WT_SUCCESS or specific error code
// Permission: accept_reco (VALIDATOR, ADMIN)
WasteTrackingError reco_accept(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t reco_id);

// Reject — reason is mandatory, empty reason returns WT_ERROR_INVALID_PARAM
// Returns WT_SUCCESS or specific error code
// Permission: reject_reco (VALIDATOR, ADMIN)
WasteTrackingError reco_reject(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t reco_id,
                               const char *reason);

// ----------------------------------------------------------------------------
// History sidebar
// Returns validator's own history — records they approved or rejected.
// status: "approved" | "rejected" | "all"
// Pass nullptr or "all" for no filter.
// Returns heap-allocated JSON array — Dart frees with reco_free_string()
// Permission: view_data (any valid session)
// ----------------------------------------------------------------------------
char *reco_my_list(WasteTrackingHandle handle, UserHandle user_handle,
                   const char *status);

// ----------------------------------------------------------------------------
// Correction
// Validator clicks "Correct" on a card → this restarts reco from scratch.
// Server atomically: updates record + logs correction + flags it + emits
// signal. changes_json: partial JSON with only the fields to correct.
//   e.g. "{\"new_weight\":12.5,\"new_gross\":15.0,\"new_tare\":2.5}"
// reason is mandatory.
// Returns WT_SUCCESS or specific error code
// Permission: correct_record (all 4 roles)
// ----------------------------------------------------------------------------
WasteTrackingError reco_correct(WasteTrackingHandle handle,
                                UserHandle user_handle, int32_t reco_id,
                                const char *reason, const char *changes_json);

void notify_reco(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // RECONCILIATION_H