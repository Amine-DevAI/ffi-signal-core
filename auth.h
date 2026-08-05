#ifndef AUTH_H
#define AUTH_H

#include "context.h"
#include "database.h"
#include "socket_connection.h"
#include "types.h"
#include <atomic>
#include <memory>
#include <string>

// ============================================================================
// UserSession
// Heap-allocated at login. Dart holds it as opaque UserHandle.
// Freed ONLY by auth_logout() — never free it yourself.
//
// Memory contract:
//   All std::string fields live on the heap inside this struct.
//   Dart never touches them directly — only through char* accessors.
//   char* accessors heap-allocate a copy — caller frees with
//   auth_free_string(). int32_t / enum accessors are value types — no free
//   needed.
// ============================================================================
struct UserSession {
  // -------------------------------------------------------------------------
  // Core identity — set at login, immutable for session lifetime
  // -------------------------------------------------------------------------
  int32_t user_id = -1; // users.id
  int32_t zone_id = -1; // users.id_zone — operator home zone, -1 if floating

  std::string username;  // users.username
  std::string full_name; // users.full_name
  std::string role_str;  // raw role string from server: "admin"/"operator"/etc
  std::string
      capabilities; // "Ghost String" — comma/pipe-separated caps from server
  UserRole role = USER_ROLE_OPERATOR; // mapped enum — used by checkPermission

  // Audit info — displayed in admin panel, never mutated after login
  std::string created_at; // users.created_at
  std::string last_login; // users.last_login

  // -------------------------------------------------------------------------
  // Session state
  // -------------------------------------------------------------------------
  std::string session_token; // server-issued token
  std::string server_url;    // needed so sockets can reconnect
  std::string station_id;    // filled AFTER heartbeat IDENTIFIED fires

  // -------------------------------------------------------------------------
  // Runtime guards
  // -------------------------------------------------------------------------
  std::atomic<bool> is_valid{false};
  // Set to true the moment logout starts (either Dart-initiated or
  // FORCE_LOGOUT from server). compare_exchange prevents double-free
  // when both paths race simultaneously.
  std::atomic<bool> logout_in_progress{false};

  // -------------------------------------------------------------------------
  // Back-pointer — lets FFI free functions reach the manager
  // Never owned by UserSession, never deleted here
  // -------------------------------------------------------------------------
  class AuthManager *auth_manager = nullptr;

  // -------------------------------------------------------------------------
  // Socket bundle
  // Moved in from WasteTrackingContext at login.
  // Owns all 4 sockets: signals, heartbeat, reco bridge, ship bridge.
  // Destroyed automatically when UserSession is deleted (logout).
  // -------------------------------------------------------------------------
  std::unique_ptr<SocketConnection> sockets;

  // Notification callbacks — one per chamber
  AuthNotifyCallback notify_cb = nullptr;              // auth
  CatalogNotifyCallback catalog_notify_cb = nullptr;   // catalog
  ZoneNotifyCallback zone_notify_cb = nullptr;         // zone
  AdminNotifyCallback admin_notify_cb = nullptr;       // admin
  WeighingNotifyCallback weighing_notify_cb = nullptr; // weighing
  RecoNotifyCallback reco_notify_cb = nullptr;         // reco
  ShipNotifyCallback ship_notify_cb = nullptr;         // shipment
  DenatNotifyCallback denat_notify_cb = nullptr;       // denaturation
  PendingNotifyCallback pending_notify_cb = nullptr;   // pending

  UserSession() = default;

  // Non-copyable, non-movable — pointer stability is required
  UserSession(const UserSession &) = delete;
  UserSession &operator=(const UserSession &) = delete;
};

// ============================================================================
// AuthManager
// Pure logic layer. Takes Database* — never owns it.
// No caching. No state beyond last_error_.
// Thread safety: all public methods are guarded by the caller's mutex
// (WasteTrackingContext::mutex_auth). Do not add internal locking here.
// ============================================================================
class AuthManager {
public:
  explicit AuthManager(Database *db);

  // --------------------------------------------------------------------------
  // Session lifecycle
  // --------------------------------------------------------------------------

  // Builds a UserSession on the heap and returns it as UserHandle.
  // password is plain text — server hashes it, we never hash client-side.
  // socket_conn is moved in — context immediately gets a fresh one.
  // Returns nullptr on any failure — call getLastError() for the reason.
  UserHandle login(const std::string &username, const std::string &password,
                   std::unique_ptr<SocketConnection> socket_conn,
                   const std::string &server_url);

  // Safely tears down the session regardless of which thread calls it.
  // Sets is_valid=false, disconnects sockets, deletes UserSession.
  // Safe to call from Dart thread after receiving FORCE_LOGOUT signal.
  void logout(UserHandle h);

  // --------------------------------------------------------------------------
  // Permission gate
  // Returns a static string literal — never free it.
  //
  // Return values:
  //   "OK"            → session alive, role allowed
  //   "NO_SESSION"    → null handle passed
  //   "SESSION_DEAD"  → is_valid = false (logged out or force-killed)
  //   "ROLE_DENIED"   → role does not have this permission
  //
  // Permission strings:
  //   "manage_users"      → ADMIN only
  //   "create_weighing"   → ADMIN, OPERATOR
  //   "submit_reco"       → ADMIN, OPERATOR
  //   "accept_reco"       → ADMIN, VALIDATOR
  //   "reject_reco"       → ADMIN, VALIDATOR
  //   "dispatch_shipment" → ADMIN, VALIDATOR
  //   "denaturation"      → ADMIN, COORDINATEUR
  //   "correct_record"    → ADMIN, OPERATOR, VALIDATOR, COORDINATEUR
  //   "manage_flags"      → ADMIN only
  //   "view_data"         → ALL roles
  // --------------------------------------------------------------------------
  const char *checkPermission(UserHandle h, const std::string &permission);

  // --------------------------------------------------------------------------
  // Signal handler — called by socket layer, auth owns these signals only:
  //   USER_CREATED, USER_UPDATED, USER_DEACTIVATED, FORCE_LOGOUT
  // All other signals are routed to their own chamber by the socket layer.
  // --------------------------------------------------------------------------
  void handleSignal(UserHandle h, const std::string &category,
                    const json &data);

  // --------------------------------------------------------------------------
  // User management — all require "manage_users" permission
  // --------------------------------------------------------------------------

  // Returns heap-allocated JSON string — caller frees with auth_free_string()
  char *listUsers(UserHandle h);

  // Returns heap-allocated JSON string with created user data on success,
  // or JSON error object on failure — caller frees with auth_free_string()
  // password is plain text
  // capabilities is the "Ghost String" — permission tokens like "weigh_material
  // | bypass_reco"
  char *createUser(UserHandle h, const std::string &username,
                   const std::string &password, const std::string &full_name,
                   const std::string &role, const std::string &capabilities,
                   int32_t zone_id);

  // fields_json: partial JSON object with only the fields to update
  // e.g. {"full_name":"Ahmed"} or {"role":"validator","id_zone":2}
  // Returns WT_SUCCESS or specific error code
  WasteTrackingError updateUser(UserHandle h, int32_t target_id,
                                const std::string &fields_json);

  // Soft delete — server sets is_active=false and kills active sessions
  WasteTrackingError deleteUser(UserHandle h, int32_t target_id);

  // password is plain text
  WasteTrackingError resetPassword(UserHandle h, int32_t target_id,
                                   const std::string &new_password);

  // --------------------------------------------------------------------------
  // Sessions & logs — all require "manage_users" permission
  // --------------------------------------------------------------------------

  // Returns heap-allocated JSON string — caller frees with auth_free_string()
  char *listSessions(UserHandle h);

  // Deactivates all active sessions for target_user_id
  // Server also fires FORCE_LOGOUT via heartbeat to the target
  WasteTrackingError deactivateSession(UserHandle h, int32_t target_user_id);

  // Returns heap-allocated JSON string — caller frees with auth_free_string()
  char *getLogs(UserHandle h);

  // --------------------------------------------------------------------------
  // Capability checking — queries the "Ghost String"
  // --------------------------------------------------------------------------

  // Returns true if the user's capabilities string contains the requested
  // capability Example: hasCapability(h, "weigh_material") checks if
  // "weigh_material" is in the string Returns false if session is invalid or
  // capability not found
  bool hasCapability(UserHandle h, const std::string &capability_name);

  // --------------------------------------------------------------------------
  std::string getLastError() const { return last_error_; }

private:
  Database *db_;
  std::string last_error_;

  // Internal helpers
  static UserRole map_role(const std::string &role_str);
};

// ============================================================================
// AUTH FFI — THE ONLY SURFACE DART SEES
//
// Memory contract summary:
//   char* return values → heap allocated, caller MUST free with
//   auth_free_string() const char* return  → static literal, NEVER free int32_t
//   / enum      → value type, no free UserHandle          → opaque pointer,
//   freed ONLY by auth_logout() WasteTrackingHandle → owned by context, never
//   freed here
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Memory
// Call this on every char* you receive from auth functions.
// Safe to call with nullptr.
// ----------------------------------------------------------------------------
void auth_free_string(char *ptr);

// ----------------------------------------------------------------------------
// Session lifecycle
// ----------------------------------------------------------------------------

// Login — consumes the socket bundle from context, returns UserHandle.
// Returns nullptr on failure. Call context_get_error() for the reason.
// password is plain text — never hash on client side.
UserHandle auth_login(WasteTrackingHandle handle, const char *username,
                      const char *password);

// Logout — safe to call from any thread.
// Handles the race between Dart-initiated logout and FORCE_LOGOUT from server.
// After this call the UserHandle is invalid — never use it again.
void auth_logout(UserHandle user_handle);

// ----------------------------------------------------------------------------
// Notification
// Register BEFORE auth_sockets_connect() — or you will miss early signals.
// Dart MUST use NativeCallable.permanent() to prevent GC collection.
// Call NativeCallable.close() after auth_logout() to release it.
// cb fires on a background thread — dispatch to main thread in Dart.
// ----------------------------------------------------------------------------
void auth_set_notify_callback(UserHandle user_handle, AuthNotifyCallback cb);

// ----------------------------------------------------------------------------
// QR bridge scan callbacks
// Register BEFORE auth_sockets_connect() — or you will miss early scans.
// cb fires on the socket thread — Dart MUST dispatch to main thread.
// Call NativeCallable.close() on logout to release each one.
// ----------------------------------------------------------------------------
void auth_set_reco_scan_callback(UserHandle user_handle, ScanCallback cb);
void auth_set_ship_scan_callback(UserHandle user_handle, ScanCallback cb);
void auth_set_denat_scan_callback(UserHandle user_handle, ScanCallback cb);

// ----------------------------------------------------------------------------
// Socket control
// Call in this order after auth_login():
//   1. auth_set_notify_callback()
//   2. auth_sockets_connect()
// ----------------------------------------------------------------------------

// Returns 1 on success, 0 on failure
int32_t auth_sockets_connect(UserHandle user_handle);
void auth_sockets_disconnect(UserHandle user_handle);

// Returns 1 if all sockets are connected, 0 otherwise
int32_t auth_sockets_ready(UserHandle user_handle);

// ----------------------------------------------------------------------------
// Permission gate
// Returns static string literal — NEVER free it.
// "OK" | "NO_SESSION" | "SESSION_DEAD" | "ROLE_DENIED"
// ----------------------------------------------------------------------------
const char *auth_check_permission(UserHandle user_handle,
                                  const char *permission);

// Capability checking — queries the "Ghost String"
// Returns 1 if capability found, 0 otherwise
// Handles both pipe-delimited (|) and comma-delimited formats
int auth_has_capability(UserHandle user_handle, const char *capability_name);

// Get user's capabilities string (heap-allocated)
// Caller must free with auth_free_string()
// Returns nullptr if session is invalid
char *auth_get_capabilities(UserHandle user_handle);

// ----------------------------------------------------------------------------
// Identity accessors — value types, no allocation, no free
// ----------------------------------------------------------------------------
int32_t auth_get_user_id(UserHandle user_handle);
int32_t auth_get_zone_id(UserHandle user_handle);
UserRole auth_get_role(UserHandle user_handle);

// ----------------------------------------------------------------------------
// Identity accessors — heap allocated, caller frees with auth_free_string()
// Returns nullptr if session is invalid
// ----------------------------------------------------------------------------
char *auth_get_username(UserHandle user_handle);
char *auth_get_full_name(UserHandle user_handle);
char *auth_get_station_id(UserHandle user_handle); // filled after IDENTIFIED
char *auth_get_created_at(UserHandle user_handle);
char *auth_get_last_login(UserHandle user_handle);

// ----------------------------------------------------------------------------
// User management — all require ADMIN role
// ----------------------------------------------------------------------------

// Returns heap-allocated JSON array string — free with auth_free_string()
// On permission denied: returns JSON error object, not nullptr
char *user_list(WasteTrackingHandle handle, UserHandle user_handle);

// Returns heap-allocated JSON object with created user or error
// role must be one of: "admin", "operator", "validator", "Coordinateur"
// capabilities is the "Ghost String" — permission tokens like "weigh_material |
// bypass_reco" zone_id: pass -1 if user is not tied to a zone
char *user_create(WasteTrackingHandle handle, UserHandle user_handle,
                  const char *username, const char *password,
                  const char *full_name, const char *role,
                  const char *capabilities, int32_t zone_id);

// fields_json: partial JSON — only include fields you want to change
// e.g. "{\"full_name\":\"Ahmed\",\"role\":\"validator\"}"
// Returns WT_SUCCESS or error code
int32_t user_update(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t target_user_id, const char *fields_json);

// Soft delete — server deactivates account and kills active sessions
// Returns WT_SUCCESS or error code
int32_t user_delete(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t target_user_id);

// password is plain text
// Returns WT_SUCCESS or error code
int32_t user_reset_password(WasteTrackingHandle handle, UserHandle user_handle,
                            int32_t target_user_id, const char *new_password);

// ----------------------------------------------------------------------------
// Sessions
// ----------------------------------------------------------------------------

// Returns heap-allocated JSON array — free with auth_free_string()
char *session_list(WasteTrackingHandle handle, UserHandle user_handle);

// Deactivates all sessions for target_user_id
// Server sends FORCE_LOGOUT to that user via heartbeat
// Returns WT_SUCCESS or error code
int32_t session_deactivate(WasteTrackingHandle handle, UserHandle user_handle,
                           int32_t target_user_id);

// ----------------------------------------------------------------------------
// Logs
// ----------------------------------------------------------------------------

// Returns heap-allocated JSON array — free with auth_free_string()
char *logs_list(WasteTrackingHandle handle, UserHandle user_handle);

#ifdef __cplusplus
}
#endif

#endif // AUTH_H