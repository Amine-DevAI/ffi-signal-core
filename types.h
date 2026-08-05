#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// OPAQUE HANDLES
// ============================================================================
typedef void *WasteTrackingHandle;
typedef void *UserHandle;
typedef void *ReconciliationHandle;
typedef void *ZoneHandle;
typedef void *ShipmentHandle;
typedef void *FlagHandle;

// ============================================================================
// ERROR CODES
// ============================================================================
typedef enum {
  WT_SUCCESS = 0,
  WT_ERROR_INVALID_PARAM = -1,
  WT_ERROR_NOT_FOUND = -2,
  WT_ERROR_DATABASE = -3,
  WT_ERROR_AUTHENTICATION = -4,
  WT_ERROR_PERMISSION = -5,
  WT_ERROR_INVALID_STATE = -6,
  WT_ERROR_IO = -7
} WasteTrackingError;

// ============================================================================
// USER ROLES
// ============================================================================
typedef enum {
  USER_ROLE_ADMIN = 0,
  USER_ROLE_OPERATOR = 1,
  USER_ROLE_VALIDATOR = 2,
  USER_ROLE_COORDINATEUR = 3,
  USER_ROLE_VIEWER = 4
} UserRole;

// ============================================================================
// SOCKET SIGNAL CALLBACKS
// Plain C function pointers — safe across the FFI boundary.
// Dart registers these once after login; they fire for the session lifetime.
// ============================================================================

// Generic chamber notification callbacks — one per domain.
// signal_name is a static literal — Dart MUST NOT free it.
typedef void (*AuthNotifyCallback)(const char *signal_name);
typedef void (*CatalogNotifyCallback)(const char *signal_name);
typedef void (*ZoneNotifyCallback)(const char *signal_name);
typedef void (*AdminNotifyCallback)(const char *signal_name);
typedef void (*WeighingNotifyCallback)(const char *signal_name);
typedef void (*RecoNotifyCallback)(const char *signal_name);
typedef void (*ShipNotifyCallback)(const char *signal_name);
typedef void (*DenatNotifyCallback)(const char *signal_name);
typedef void (*PendingNotifyCallback)(const char *signal_name);

// Fired when a new weighing is created by any operator
typedef void (*WeighingCallback)(int32_t weighing_id, int32_t operator_id,
                                 double net_weight, const char *lot_number);

// Fired when a validator approves a reconciliation → shipment created
typedef void (*ShipmentReadyCallback)(int32_t reco_id, int32_t shipment_id,
                                      int32_t validator_id);

// Fired when a shipment is dispatched or cancelled
typedef void (*ShipmentChangedCallback)(int32_t shipment_id, int32_t shipper_id,
                                        const char *status, const char *note);

// Fired when an operator raises a flag
typedef void (*FlagCreatedCallback)(int32_t flag_id, const char *table_name,
                                    int32_t record_id, const char *reason);

// Fired when admin approves or rejects a flag
typedef void (*FlagReviewedCallback)(
    int32_t flag_id,
    const char *status,  // "approved" | "rejected"
    const char *reason); // empty on approve

// Fired when an operator submits a correction after flag approval
typedef void (*FlagCorrectedCallback)(int32_t flag_id, const char *table_name,
                                      const char *field_name,
                                      const char *old_value,
                                      const char *new_value);

// Fired for REFRESH_USERS / REFRESH_ZONES / REFRESH_PRODUCTS / REFRESH_TYPES
// entity will be "USERS", "ZONES", "PRODUCTS", or "TYPES"
typedef void (*RefreshCallback)(const char *entity);

// Fired when the server force-kills this session (admin deactivated it)
typedef void (*ForceLogoutCallback)(const char *reason);

// Fired once after login when the heartbeat socket confirms identity
// station_id = the machine-id fingerprint the server echoed back
typedef void (*IdentifiedCallback)(const char *station_id,
                                   const char *display_name);

// QR scan from phone — two independent bridges
typedef void (*ScanCallback)(const char *qr_data);

// Fallback — fires for any signal category not handled by a typed callback
typedef void (*RawSignalCallback)(const char *json_payload);

#ifdef __cplusplus
}
#endif

#endif // TYPES_H