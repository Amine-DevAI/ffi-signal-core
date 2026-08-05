#ifndef PENDING_H
#define PENDING_H

#include "types.h"
#include <cstdint>

// ============================================================================
// PENDING CHAMBER
//
// The live work queues. What needs to be done RIGHT NOW on the factory floor.
// Each role sees only their own queue — the work assigned to them.
// These run completely independently and never block other chambers.
//
// Three independent queues:
//   pending_reco_list  → Validator's queue  — items waiting to be scanned
//                        and verified (status: 'pending' or 'scanned')
//   pending_ship_list  → Shipper's queue    — approved batches waiting to
//                        be loaded onto the truck (status: 'pending')
//   pending_denat_list → Coordinateur's queue — batches requiring chemical
//                        stabilization before shipment (status: 'pending')
//
// These are NOT history. History lives in each chamber's my_list function.
// Pending = work to do. My_list = work already done.
//
// Permission model:
//   pending_reco_list  → accept_reco    (VALIDATOR, ADMIN)
//   pending_ship_list  → dispatch_shipment (VALIDATOR, ADMIN)
//   pending_denat_list → denaturation   (COORDINATEUR, ADMIN)
//
// Memory contract:
//   char*              → heap allocated. Dart MUST free with
//   pending_free_string(). WasteTrackingError → value type, no free needed.
//
// Signal contract:
//   RECO_STEP_COMPLETED  → Dart calls pending_reco_list()
//   SHIPMENT_READY       → Dart calls pending_ship_list()
//   DENATURATION_PENDING → Dart calls pending_denat_list()
//   notify_pending() is called by the socket chamber for all three.
//   Dart matches signal_name to the right list function.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// VALIDATOR QUEUE
//
// pending_reco_list
//   Returns all reconciliations with status 'pending' or 'scanned'.
//   Ordered by created_at ASC — oldest first (FIFO, fair queue).
//   Each item contains full context: original weights, material, product,
//   lot, QR code, is_flagged — everything the validator needs to start work.
//   No filter needed — validator sees the entire facility queue.
//
//   Returns heap-allocated JSON array string. Free with pending_free_string().
//   Returns error JSON on failure — never nullptr on reachable server.
//
// Permission: accept_reco (VALIDATOR, ADMIN)
// ---------------------------------------------------------------------------
char *pending_reco_list(WasteTrackingHandle handle, UserHandle user_handle);

// ---------------------------------------------------------------------------
// SHIPPER QUEUE
//
// pending_ship_list
//   Returns all shipments with status 'pending'.
//   Ordered by zone ASC, created_at ASC — grouped by zone for warehouse flow.
//   Each item contains verified weights (from reconciliation), material,
//   product, lot, zone, is_flagged — everything the shipper needs to
//   confirm before loading.
//
//   Returns heap-allocated JSON array string. Free with pending_free_string().
//   Returns error JSON on failure — never nullptr on reachable server.
//
// Permission: dispatch_shipment (VALIDATOR, ADMIN)
// ---------------------------------------------------------------------------
char *pending_ship_list(WasteTrackingHandle handle, UserHandle user_handle);

// ---------------------------------------------------------------------------
// COORDINATEUR QUEUE
//
// pending_denat_list
//   Returns all batches requiring chemical stabilization (denaturation).
//   This is the "Mega Join" result including Origin (Zone/Operator),
//   Material (Nature/Brand), and Physics (Weights/Variance).
//
//   Flow:
//     1. Validator Accepts Reco -> Signal DENATURATION_PENDING fires.
//     2. Dart receives signal -> Calls pending_denat_list().
//     3. Coordinator selects item -> Proceeds to denaturation chamber.
//
//   Returns heap-allocated JSON string. Dart MUST free with
//   pending_free_string().
//
// Permission: denaturation (COORDINATEUR, ADMIN)
// ---------------------------------------------------------------------------
char *pending_denat_list(WasteTrackingHandle handle, UserHandle user_handle);
// ---------------------------------------------------------------------------
// MEMORY
//
// pending_free_string
//   Dart MUST call this on every char* received from pending functions.
//   Call it when the data is no longer needed — widget disposed, list
//   refreshed, screen closed. Dart owns the timing.
//   Safe to call with nullptr — does nothing.
// ---------------------------------------------------------------------------
void pending_free_string(char *ptr);

// ---------------------------------------------------------------------------
// NOTIFICATION
//
// pending_set_notify_callback
//   Dart calls this ONCE after login, before the socket connects.
//   One callback covers all three queues — Dart matches signal_name
//   to the right list function.
//   Dart MUST use NativeCallable.permanent() — prevents GC collection.
//   Call NativeCallable.close() on logout to release it.
//   cb fires on the socket thread — Dart MUST dispatch to main thread.
//
// notify_pending
//   Called by the socket chamber when a pending-relevant signal arrives.
//   signal_name is one of:
//     "RECO_STEP_COMPLETED"  → Dart calls pending_reco_list()
//     "SHIPMENT_READY"       → Dart calls pending_ship_list()
//     "DENATURATION_PENDING" → Dart calls pending_denat_list()
//   signal_name is a static literal — Dart must NOT free it.
//   Safe to call from any thread. Never fires after session is dead.
// ---------------------------------------------------------------------------
void pending_set_notify_callback(UserHandle user_handle,
                                 PendingNotifyCallback cb);

void notify_pending(UserHandle user_handle, const char *signal_name);

#ifdef __cplusplus
}
#endif

#endif // PENDING_H