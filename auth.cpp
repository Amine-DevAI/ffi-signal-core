#include "auth.h"
#include <cstring>
#include <iostream>
#include <mutex>

// ============================================================================
// Internal helper — heap allocates a C string copy for FFI transfer.
// Dart receives the pointer and MUST free it with auth_free_string().
// Returns nullptr if input is empty.
// ============================================================================
static char *alloc_str(const std::string &s) {
  if (s.empty())
    return nullptr;
  char *p = static_cast<char *>(std::malloc(s.length() + 1));
  if (p)
    std::memcpy(p, s.c_str(), s.length() + 1);
  return p;
}

// ============================================================================
// Role mapping — server sends lowercase string, we map to enum
// ============================================================================
UserRole AuthManager::map_role(const std::string &role_str) {
  if (role_str == "admin")
    return USER_ROLE_ADMIN;
  if (role_str == "operator")
    return USER_ROLE_OPERATOR;
  if (role_str == "validator")
    return USER_ROLE_VALIDATOR;
  if (role_str == "coordinateur" || role_str == "Coordinateur")
    return USER_ROLE_COORDINATEUR;
  // Unknown role — safest fallback is operator (least privilege that can work)
  std::cerr << "[Auth] Unknown role string: '" << role_str
            << "' — defaulting to OPERATOR" << std::endl;
  return USER_ROLE_OPERATOR;
}

// ============================================================================
// Constructor
// ============================================================================
AuthManager::AuthManager(Database *db) : db_(db) {}

// ============================================================================
// Login
// ============================================================================
UserHandle AuthManager::login(const std::string &username,
                              const std::string &password,
                              std::unique_ptr<SocketConnection> socket_conn,
                              const std::string &server_url) {
  if (!db_) {
    last_error_ = "Database not initialized.";
    return nullptr;
  }

  if (username.empty() || password.empty()) {
    last_error_ = "Username and password are required.";
    return nullptr;
  }

  // Password goes plain text — server hashes it server-side.
  // We NEVER hash on client side.
  json response = db_->api_auth_Login(username, password);

  if (!response.value("success", false)) {
    last_error_ = response.value("error_message", "Login failed.");

    // Map server error messages to specific error context for logging
    const std::string &err = last_error_;
    if (err.find("deactivated") != std::string::npos)
      std::cerr << "[Auth] Login rejected — account disabled: " << username
                << std::endl;
    else if (err.find("Invalid credentials") != std::string::npos)
      std::cerr << "[Auth] Login rejected — wrong credentials: " << username
                << std::endl;
    else
      std::cerr << "[Auth] Login failed: " << err << std::endl;

    return nullptr;
  }

  const json &d = response.value("data", json::object());

  // Validate the response has everything we need
  if (!d.contains("id") || !d.contains("session_token")) {
    last_error_ = "Malformed login response — missing id or session_token.";
    std::cerr << "[Auth] " << last_error_ << std::endl;
    return nullptr;
  }

  int32_t user_id = d.value("id", -1);
  std::string token = d.value("session_token", "");

  if (user_id == -1 || token.empty()) {
    last_error_ = "Server returned invalid user_id or empty token.";
    return nullptr;
  }

  // Build the session on the heap
  // Dart will hold this as an opaque UserHandle until auth_logout()
  auto *s = new (std::nothrow) UserSession();
  if (!s) {
    last_error_ = "Out of memory allocating UserSession.";
    return nullptr;
  }

  // Identity — from users table
  s->user_id = user_id;
  s->zone_id = d.value("id_zone", -1); // -1 = floating (no zone assignment)
  s->username = d.value("username", username);
  s->full_name = d.value("full_name", "");
  s->role_str = d.value("role", "operator");
  s->capabilities =
      d.value("capabilities", ""); // Ghost String — permission tokens
  s->role = map_role(s->role_str);
  s->created_at = d.value("created_at", "");
  s->last_login = d.value("last_login", "");

  // Session
  s->session_token = token;
  s->server_url = server_url;
  s->station_id = ""; // filled later when IDENTIFIED fires from heartbeat

  // Runtime
  s->is_valid = true;
  s->logout_in_progress = false;
  s->auth_manager = this;

  // Take ownership of the socket bundle from context
  // Context immediately gets a fresh SocketConnection after this
  s->sockets = std::move(socket_conn);

  std::cout << "[Auth] Login OK — " << s->username << " (role=" << s->role_str
            << ", id=" << user_id << ", zone=" << s->zone_id
            << ", caps=" << (s->capabilities.empty() ? "none" : s->capabilities)
            << ")" << std::endl;

  return static_cast<UserHandle>(s);
}

// ============================================================================
// Logout
// Deadlock-safe — can be called from Dart thread OR triggered by FORCE_LOGOUT.
// The atomic flag ensures only one path wins the race.
// ============================================================================
void AuthManager::logout(UserHandle h) {
  if (!h)
    return;

  auto *s = static_cast<UserSession *>(h);

  // Atomic gate — only one thread can proceed past here
  bool expected = false;
  if (!s->logout_in_progress.compare_exchange_strong(expected, true)) {
    // Another thread already started logout — do nothing
    std::cerr << "[Auth] Logout already in progress for user " << s->user_id
              << " — skipping duplicate." << std::endl;
    return;
  }

  std::cout << "[Auth] Logging out user " << s->username
            << " (id=" << s->user_id << ")" << std::endl;

  // Mark dead FIRST — all other FFI calls will return immediately after this
  s->is_valid = false;

  // Disconnect sockets cleanly
  // disconnect_all() is safe to call from any thread and safe to call
  // even if already disconnected — it checks internal state before acting
  if (s->sockets) {
    s->sockets->disconnect_all();
  }

  // Null the callback before delete to prevent any late-firing signal
  // from calling into freed memory
  s->notify_cb = nullptr;

  delete s;
}

// ============================================================================
// Permission gate
// Returns static string literals — never heap allocated, never free them.
// ============================================================================
const char *AuthManager::checkPermission(UserHandle h,
                                         const std::string &permission) {
  if (!h)
    return "NO_SESSION";

  auto *s = static_cast<UserSession *>(h);

  if (!s->is_valid || s->logout_in_progress)
    return "SESSION_DEAD";

  // Admin passes everything
  if (s->role == USER_ROLE_ADMIN)
    return "OK";

  // ---- Permission table ----

  if (permission == "view_data")
    return "OK"; // all roles can view

  if (permission == "manage_users" || permission == "manage_flags")
    return "ROLE_DENIED"; // admin only, already handled above

  if (permission == "create_weighing" || permission == "submit_reco") {
    if (s->role == USER_ROLE_OPERATOR)
      return "OK";
    return "ROLE_DENIED";
  }

  if (permission == "accept_reco" || permission == "reject_reco" ||
      permission == "dispatch_shipment") {
    if (s->role == USER_ROLE_VALIDATOR)
      return "OK";
    return "ROLE_DENIED";
  }

  if (permission == "denaturation") {
    if (s->role == USER_ROLE_COORDINATEUR)
      return "OK";
    return "ROLE_DENIED";
  }

  if (permission == "correct_record") {
    // Everyone except viewer (no viewer role exists — all 4 roles can correct)
    return "OK";
  }

  // New screen-based capability ids from the Flutter admin UI.
  // If the user has the matching ghost capability string, allow it.
  if (permission == "weigh_1" || permission == "reconcile_2" ||
      permission == "shipment_3" || permission == "denaturation_4" ||
      permission == "dashboard_11" || permission == "user_admin_5" ||
      permission == "catalog_6" || permission == "sessions_7" ||
      permission == "oversight_8" || permission == "corrections_9" ||
      permission == "export_10") {
    return hasCapability(h, permission) ? "OK" : "ROLE_DENIED";
  }

  // Unknown permission string — deny by default
  std::cerr << "[Auth] Unknown permission queried: '" << permission << "'"
            << std::endl;
  return "ROLE_DENIED";
}

// ============================================================================
// Signal handler
// Called by the socket layer for signals auth chamber owns.
// Auth owns: USER_CREATED, USER_UPDATED, USER_DEACTIVATED, FORCE_LOGOUT
// All other signals are routed to their own chamber.
// ============================================================================
void AuthManager::handleSignal(UserHandle h, const std::string &category,
                               const json &data) {
  if (!h)
    return;
  auto *s = static_cast<UserSession *>(h);

  // FORCE_LOGOUT — server killed this session
  if (category == "FORCE_LOGOUT") {
    std::cerr << "[Auth] FORCE_LOGOUT received for user " << s->user_id
              << std::endl;

    // Mark dead immediately — blocks all other FFI calls
    s->is_valid = false;

    // Fire Dart notification BEFORE disconnecting sockets.
    // Dart will call auth_logout() in response — that handles disconnect.
    // We NEVER call disconnect_all() from inside a socket callback — deadlock.
    if (s->notify_cb)
      s->notify_cb("FORCE_LOGOUT");

    return;
  }

  // USER_CREATED / USER_UPDATED / USER_DEACTIVATED
  // Session stays alive — just tell Dart to refresh the user list
  if (category == "USER_CREATED" || category == "USER_UPDATED" ||
      category == "USER_DEACTIVATED") {

    if (!s->is_valid)
      return; // session already dead, don't fire

    if (s->notify_cb)
      s->notify_cb(category.c_str());

    return;
  }

  // Anything else should never reach here — log and ignore
  std::cerr << "[Auth] handleSignal received unexpected category: " << category
            << std::endl;
}

// ============================================================================
// User management
// ============================================================================

char *AuthManager::listUsers(UserHandle h) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "OK") != 0) {
    // Return a JSON error object — Dart can parse it
    std::string err =
        "{\"success\":false,\"error\":\"" + std::string(perm) + "\"}";
    return alloc_str(err);
  }

  json result = db_->api_admin_ListUsers();
  return alloc_str(result.dump());
}

char *AuthManager::createUser(UserHandle h, const std::string &username,
                              const std::string &password,
                              const std::string &full_name,
                              const std::string &role,
                              const std::string &capabilities,
                              int32_t zone_id) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "OK") != 0) {
    std::string err =
        "{\"success\":false,\"error\":\"" + std::string(perm) + "\"}";
    return alloc_str(err);
  }

  if (username.empty() || password.empty() || full_name.empty() ||
      role.empty() || capabilities.empty()) {
    return alloc_str("{\"success\":false,\"error\":\"Missing required fields "
                     "(including capabilities)\"}");
  }

  // Validate role string matches DB CHECK constraint
  if (role != "admin" && role != "operator" && role != "validator" &&
      role != "Coordinateur") {
    return alloc_str(
        "{\"success\":false,\"error\":\"Invalid role — must be admin, "
        "operator, validator, or Coordinateur\"}");
  }

  // password plain text — server hashes it
  // capabilities is the "Ghost String" — forwarded to database layer
  json result =
      db_->api_admin_CreateUser(username, password, full_name, role,
                                capabilities, zone_id > 0 ? zone_id : -1);
  return alloc_str(result.dump());
}

WasteTrackingError AuthManager::updateUser(UserHandle h, int32_t target_id,
                                           const std::string &fields_json) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  if (target_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  json fields;
  try {
    fields = json::parse(fields_json);
  } catch (...) {
    last_error_ = "Invalid JSON in fields_json.";
    return WT_ERROR_INVALID_PARAM;
  }

  bool ok = db_->api_admin_UpdateUser(target_id, fields);
  return ok ? WT_SUCCESS : WT_ERROR_DATABASE;
}

WasteTrackingError AuthManager::deleteUser(UserHandle h, int32_t target_id) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  if (target_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  // Guard: admin cannot delete themselves
  auto *s = static_cast<UserSession *>(h);
  if (target_id == s->user_id) {
    last_error_ = "Cannot delete your own account.";
    return WT_ERROR_INVALID_PARAM;
  }

  bool ok = db_->api_admin_DeleteUser(target_id);
  return ok ? WT_SUCCESS : WT_ERROR_DATABASE;
}

WasteTrackingError AuthManager::resetPassword(UserHandle h, int32_t target_id,
                                              const std::string &new_password) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  if (target_id <= 0 || new_password.empty())
    return WT_ERROR_INVALID_PARAM;

  // Pass plain text password — server hashes it via the update endpoint
  json fields = {{"password", new_password}};
  bool ok = db_->api_admin_UpdateUser(target_id, fields);
  return ok ? WT_SUCCESS : WT_ERROR_DATABASE;
}

char *AuthManager::listSessions(UserHandle h) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "OK") != 0) {
    std::string err =
        "{\"success\":false,\"error\":\"" + std::string(perm) + "\"}";
    return alloc_str(err);
  }

  json result = db_->api_admin_ListSessions();
  return alloc_str(result.dump());
}

WasteTrackingError AuthManager::deactivateSession(UserHandle h,
                                                  int32_t target_user_id) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(perm, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(perm, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;

  if (target_user_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  bool ok = db_->api_admin_DeactivateSession(target_user_id);
  return ok ? WT_SUCCESS : WT_ERROR_DATABASE;
}

char *AuthManager::getLogs(UserHandle h) {
  const char *perm = checkPermission(h, "manage_users");
  if (std::strcmp(perm, "OK") != 0) {
    std::string err =
        "{\"success\":false,\"error\":\"" + std::string(perm) + "\"}";
    return alloc_str(err);
  }

  json result = db_->api_admin_GetLogs();
  return alloc_str(result.dump());
}

// ============================================================================
// Capability checking — queries the "Ghost String"
// ============================================================================

bool AuthManager::hasCapability(UserHandle h,
                                const std::string &capability_name) {
  if (!h)
    return false;

  auto *s = static_cast<UserSession *>(h);

  if (!s->is_valid || s->logout_in_progress)
    return false;

  if (s->capabilities.empty())
    return false;

  // Search for the capability in the string
  // Handle both pipe-delimited (|) and comma-delimited formats
  size_t pos = 0;
  while (pos < s->capabilities.length()) {
    // Find the next separator (pipe or comma)
    size_t separator_pos = s->capabilities.find_first_of("|,", pos);
    if (separator_pos == std::string::npos)
      separator_pos = s->capabilities.length();

    // Extract and trim whitespace
    std::string token = s->capabilities.substr(pos, separator_pos - pos);
    const char *ws = " \t\n\r\f\v";
    auto first = token.find_first_not_of(ws);
    auto last = token.find_last_not_of(ws);
    if (first != std::string::npos && last != std::string::npos) {
      token = token.substr(first, last - first + 1);
      if (token == capability_name)
        return true;
    }

    // Move to next token (skip separator)
    pos = separator_pos + 1;
  }

  return false;
}

// ============================================================================
// AUTH FFI IMPLEMENTATION
// ============================================================================

extern "C" {

void auth_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ----------------------------------------------------------------------------
// Session lifecycle
// ----------------------------------------------------------------------------

UserHandle auth_login(WasteTrackingHandle handle, const char *username,
                      const char *password) {
  if (!handle || !username || !password)
    return nullptr;

  auto *ctx = static_cast<WasteTrackingContext *>(handle);

  // Ensure auth manager is ready
  if (!ctx->auth) {
    if (!ctx->db || !ctx->db->initialize())
      return nullptr;
    ctx->auth = std::make_unique<AuthManager>(ctx->db.get());
  }

  std::lock_guard<std::mutex> lock(ctx->mutex_auth);

  // Move socket bundle out of context into the new session.
  // Context immediately gets a fresh one so it's ready for the next login.
  auto sock = std::move(ctx->socket_conn);
  ctx->socket_conn = std::make_unique<SocketConnection>();

  return ctx->auth->login(std::string(username), std::string(password),
                          std::move(sock), ctx->server_url);
}

void auth_logout(UserHandle user_handle) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (s->auth_manager)
    s->auth_manager->logout(user_handle);
}

// ----------------------------------------------------------------------------
// Notification
// ----------------------------------------------------------------------------

void auth_set_notify_callback(UserHandle user_handle, AuthNotifyCallback cb) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid)
    return;
  s->notify_cb = cb;
}

// These check !s->is_valid || !s->sockets explicitly, which is robust.
void auth_set_reco_scan_callback(UserHandle user_handle, ScanCallback cb) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid || !s->sockets)
    return;
  s->sockets->onRecoScan(cb);
}

void auth_set_ship_scan_callback(UserHandle user_handle, ScanCallback cb) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid || !s->sockets)
    return;
  s->sockets->onShipScan(cb);
}

void auth_set_denat_scan_callback(UserHandle user_handle, ScanCallback cb) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid || !s->sockets)
    return;
  s->sockets->onDenatScan(cb);
}

// ----------------------------------------------------------------------------
// Socket control
// ----------------------------------------------------------------------------

int32_t auth_sockets_connect(UserHandle user_handle) {
  if (!user_handle)
    return 0;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid || !s->sockets || s->server_url.empty())
    return 0;
  bool ok = s->sockets->connect_all(s->server_url, s->user_id, s->session_token,
                                    user_handle);
  if (!ok)
    std::cerr << "[Auth] Socket connection failed for user " << s->user_id
              << std::endl;
  return ok ? 1 : 0;
}

void auth_sockets_disconnect(UserHandle user_handle) {
  if (!user_handle)
    return;
  auto *s = static_cast<UserSession *>(user_handle);
  if (s->sockets)
    s->sockets->disconnect_all();
}

int32_t auth_sockets_ready(UserHandle user_handle) {
  if (!user_handle)
    return 0;
  auto *s = static_cast<UserSession *>(user_handle);
  return (s->sockets && s->sockets->isConnected()) ? 1 : 0;
}

// ----------------------------------------------------------------------------
// Permission gate
// ----------------------------------------------------------------------------

const char *auth_check_permission(UserHandle user_handle,
                                  const char *permission) {
  if (!user_handle || !permission)
    return "NO_SESSION";
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->auth_manager)
    return "NO_SESSION";
  return s->auth_manager->checkPermission(user_handle, std::string(permission));
}

// Check if user has a specific capability in their "Ghost String"
// Returns true (1) if capability is found, false (0) otherwise
int auth_has_capability(UserHandle user_handle, const char *capability_name) {
  if (!user_handle || !capability_name)
    return 0;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->auth_manager)
    return 0;
  return s->auth_manager->hasCapability(user_handle,
                                        std::string(capability_name))
             ? 1
             : 0;
}

// Accessor — get user's capabilities string (heap-allocated)
// Caller must free with auth_free_string()
char *auth_get_capabilities(UserHandle user_handle) {
  if (!user_handle)
    return nullptr;
  auto *s = static_cast<UserSession *>(user_handle);
  if (!s->is_valid)
    return nullptr;
  return alloc_str(s->capabilities);
}

// ----------------------------------------------------------------------------
// Identity — value types
// ----------------------------------------------------------------------------

int32_t auth_get_user_id(UserHandle user_handle) {
  if (!user_handle)
    return -1;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? s->user_id : -1;
}

int32_t auth_get_zone_id(UserHandle user_handle) {
  if (!user_handle)
    return -1;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? s->zone_id : -1;
}

UserRole auth_get_role(UserHandle user_handle) {
  if (!user_handle)
    return USER_ROLE_OPERATOR;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->role;
}

// ----------------------------------------------------------------------------
// Identity — heap allocated, caller frees
// ----------------------------------------------------------------------------

char *auth_get_username(UserHandle user_handle) {
  if (!user_handle)
    return nullptr;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? alloc_str(s->username) : nullptr;
}

char *auth_get_full_name(UserHandle user_handle) {
  if (!user_handle)
    return nullptr;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? alloc_str(s->full_name) : nullptr;
}

char *auth_get_station_id(UserHandle user_handle) {
  if (!user_handle)
    return nullptr;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? alloc_str(s->station_id) : nullptr;
}

char *auth_get_created_at(UserHandle user_handle) {
  if (!user_handle)
    return nullptr;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? alloc_str(s->created_at) : nullptr;
}

char *auth_get_last_login(UserHandle user_handle) {
  if (!user_handle)
    return nullptr;
  auto *s = static_cast<UserSession *>(user_handle);
  return s->is_valid ? alloc_str(s->last_login) : nullptr;
}

// ----------------------------------------------------------------------------
// User management
// ----------------------------------------------------------------------------

char *user_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!handle || !user_handle)
    return nullptr;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return ctx->auth->listUsers(user_handle);
}

char *user_create(WasteTrackingHandle handle, UserHandle user_handle,
                  const char *username, const char *password,
                  const char *full_name, const char *role,
                  const char *capabilities, int32_t zone_id) {
  if (!handle || !user_handle || !username || !password || !full_name ||
      !role || !capabilities)
    return alloc_str(
        "{\"success\":false,\"error\":\"WT_ERROR_INVALID_PARAM\"}");
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return ctx->auth->createUser(user_handle, std::string(username),
                               std::string(password), std::string(full_name),
                               std::string(role), std::string(capabilities),
                               zone_id);
}

int32_t user_update(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t target_user_id, const char *fields_json) {
  if (!handle || !user_handle || !fields_json)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return static_cast<int32_t>(ctx->auth->updateUser(user_handle, target_user_id,
                                                    std::string(fields_json)));
}

int32_t user_delete(WasteTrackingHandle handle, UserHandle user_handle,
                    int32_t target_user_id) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return static_cast<int32_t>(
      ctx->auth->deleteUser(user_handle, target_user_id));
}

int32_t user_reset_password(WasteTrackingHandle handle, UserHandle user_handle,
                            int32_t target_user_id, const char *new_password) {
  if (!handle || !user_handle || !new_password)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return static_cast<int32_t>(ctx->auth->resetPassword(
      user_handle, target_user_id, std::string(new_password)));
}

// ----------------------------------------------------------------------------
// Sessions
// ----------------------------------------------------------------------------

char *session_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!handle || !user_handle)
    return nullptr;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return ctx->auth->listSessions(user_handle);
}

int32_t session_deactivate(WasteTrackingHandle handle, UserHandle user_handle,
                           int32_t target_user_id) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return static_cast<int32_t>(
      ctx->auth->deactivateSession(user_handle, target_user_id));
}

// ----------------------------------------------------------------------------
// Logs
// ----------------------------------------------------------------------------

char *logs_list(WasteTrackingHandle handle, UserHandle user_handle) {
  if (!handle || !user_handle)
    return nullptr;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  std::lock_guard<std::mutex> lock(ctx->mutex_auth);
  return ctx->auth->getLogs(user_handle);
}

} // extern "C"