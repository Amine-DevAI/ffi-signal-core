#ifndef CONTEXT_H
#define CONTEXT_H

#include "database.h"
#include "socket_connection.h"
#include "types.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// Forward declare — don't include auth.h here (circular dependency)
class AuthManager;

// ============================================================================
// WasteTrackingContext
// Owns the environment: db, auth manager, server config.
// One SocketConnection lives here as a "ready to use" instance.
// At login time, ownership transfers into UserSession.
// A fresh one is constructed here immediately after.
// ============================================================================
struct WasteTrackingContext {
  std::unique_ptr<Database> db;
  std::unique_ptr<AuthManager> auth;

  // The one socket bundle — handed to UserSession on login,
  // immediately replaced with a fresh one so context is always ready
  std::unique_ptr<SocketConnection> socket_conn;

  std::mutex mutex_system;
  std::mutex mutex_auth;
  std::string current_ip;
  std::string server_url;
  std::string last_error;

  explicit WasteTrackingContext(const std::string &ip);
  bool set_server(const std::string &ip);
};

// ============================================================================
// CONTEXT FFI
// ============================================================================
#ifdef __cplusplus
extern "C" {
#endif

WasteTrackingHandle context_init(const char *ip_address);
void context_cleanup(WasteTrackingHandle handle);
WasteTrackingError context_set_server(WasteTrackingHandle handle,
                                      const char *ip_address);
const char *context_get_error(WasteTrackingHandle handle);
void context_free_string(char *str);

#ifdef __cplusplus
}
#endif

#endif // CONTEXT_H