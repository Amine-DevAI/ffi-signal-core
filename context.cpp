#include "context.h"
#include "auth.h"
#include "database.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

WasteTrackingContext::WasteTrackingContext(const std::string &ip)
    : current_ip(ip) {
  server_url = "http://" + ip + ":8000";
  db = std::make_unique<Database>(server_url);
  socket_conn = std::make_unique<SocketConnection>();
}

bool WasteTrackingContext::set_server(const std::string &ip) {
  current_ip = ip;
  server_url = "http://" + ip + ":8000";
  bool db_ok = true;
  if (db) {
    db->setServerUrl(server_url);
    db_ok = db->initialize();
  }

  // Only reconnect sockets if already connected (user session active)
  if (socket_conn && socket_conn->isConnected()) {
    int user_id = socket_conn->getUserId();
    std::string token = socket_conn->getToken();
    UserHandle user_handle = socket_conn->getUserHandle();
    socket_conn->disconnect_all();
    socket_conn->connect_all(server_url, user_id, token, user_handle);
  }

  return db_ok;
}

// Resolves and validates the context pointer.
// Returns nullptr if handle is null or db is not ready.
static WasteTrackingContext *resolve_ctx(WasteTrackingHandle handle) {
  if (!handle)
    return nullptr;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->db)
    return nullptr;
  return ctx;
}

// ============================================================================
// CONTEXT FFI IMPLEMENTATION
// ============================================================================

extern "C" {

WasteTrackingHandle context_init(const char *ip_address) {
  if (!ip_address)
    return nullptr;
  auto *ctx = new WasteTrackingContext(std::string(ip_address));
  if (!ctx->db->initialize()) {
    ctx->last_error = "Failed to connect to server at " + ctx->server_url;
    // We return the handle anyway so Dart can call context_get_error()
  }
  return static_cast<WasteTrackingHandle>(ctx);
}

void context_cleanup(WasteTrackingHandle handle) {
  if (handle)
    delete static_cast<WasteTrackingContext *>(handle);
}

WasteTrackingError context_set_server(WasteTrackingHandle handle,
                                      const char *ip_address) {
  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;
  if (!ip_address)
    return WT_ERROR_INVALID_PARAM;
  return ctx->set_server(std::string(ip_address)) ? WT_SUCCESS
                                                  : WT_ERROR_DATABASE;
}

const char *context_get_error(WasteTrackingHandle handle) {
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  return (ctx && !ctx->last_error.empty()) ? ctx->last_error.c_str() : "";
}

void context_free_string(char *str) {
  if (str)
    std::free(str);
}

} // extern "C"