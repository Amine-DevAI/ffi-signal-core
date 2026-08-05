#include "database.h"
#include "httplib.h"
#include <iostream>

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

struct ParsedUrl {
  std::string host;
  int port;
  bool valid;
};

static ParsedUrl parse_url(const std::string &url) {
  auto pe = url.find("://");
  if (pe == std::string::npos)
    return {"", 0, false};
  std::string hp = url.substr(pe + 3);
  auto pp = hp.find(':');
  std::string host = (pp != std::string::npos) ? hp.substr(0, pp) : hp;
  int port = (pp != std::string::npos) ? std::stoi(hp.substr(pp + 1)) : 8000;
  return {host, port, true};
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Database::Database(const std::string &server_url) : server_url_(server_url) {}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string Database::sanitize(const std::string &raw) {
  std::string s = raw;
  // Strip UTF-8 BOM
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF)
    s = s.substr(3);
  const char *ws = " \t\n\r\f\v";
  auto first = s.find_first_not_of(ws);
  if (first == std::string::npos)
    return "";
  auto last = s.find_last_not_of(ws);
  return s.substr(first, last - first + 1);
}

json Database::make_error(const std::string &code, const std::string &msg) {
  return {{"success", false},
          {"error_code", code},
          {"error_message", msg},
          {"data", nullptr}};
}

// ---------------------------------------------------------------------------
// HTTP primitives
// ---------------------------------------------------------------------------

json Database::http_get(const std::string &endpoint) {
  try {
    auto u = parse_url(server_url_);
    if (!u.valid)
      return make_error("INVALID_URL", "Bad server URL");
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    auto res = cli.Get(endpoint);
    if (!res)
      return make_error("CONN_REFUSED", "Server unreachable: " + server_url_);

    std::string res_body = sanitize(res->body);
    if (res->status != 200) {
      try {
        return json::parse(res_body);
      } catch (...) {
        return make_error("HTTP_" + std::to_string(res->status),
                          res_body.empty()
                              ? "Status " + std::to_string(res->status)
                              : res_body);
      }
    }
    return json::parse(res_body);
  } catch (const std::exception &e) {
    return make_error("GET_ERROR", e.what());
  }
}

json Database::http_post(const std::string &endpoint, const json &body) {
  try {
    auto u = parse_url(server_url_);
    if (!u.valid)
      return make_error("INVALID_URL", "Bad server URL");
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    auto res = cli.Post(endpoint, body.dump(), "application/json");
    if (!res)
      return make_error("CONN_REFUSED", "Server unreachable: " + server_url_);
    if (res->status != 200) {
      std::string res_body = sanitize(res->body);
      try {
        return json::parse(res_body);
      } catch (...) {
        return make_error("HTTP_" + std::to_string(res->status),
                          res_body.empty()
                              ? "Status " + std::to_string(res->status)
                              : res_body);
      }
    }
    return json::parse(sanitize(res->body));
  } catch (const std::exception &e) {
    return make_error("POST_ERROR", e.what());
  }
}

json Database::http_put(const std::string &endpoint, const json &body) {
  try {
    auto u = parse_url(server_url_);
    if (!u.valid)
      return make_error("INVALID_URL", "Bad server URL");
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    auto res = cli.Put(endpoint, body.dump(), "application/json");
    if (!res)
      return make_error("CONN_REFUSED", "Server unreachable: " + server_url_);
    if (res->status != 200) {
      std::string res_body = sanitize(res->body);
      try {
        return json::parse(res_body);
      } catch (...) {
        return make_error("HTTP_" + std::to_string(res->status),
                          res_body.empty()
                              ? "Status " + std::to_string(res->status)
                              : res_body);
      }
    }
    return json::parse(sanitize(res->body));
  } catch (const std::exception &e) {
    return make_error("PUT_ERROR", e.what());
  }
}

json Database::http_delete(const std::string &endpoint) {
  try {
    auto u = parse_url(server_url_);
    if (!u.valid)
      return make_error("INVALID_URL", "Bad server URL");
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(5, 0);
    auto res = cli.Delete(endpoint);
    if (!res)
      return make_error("CONN_REFUSED", "Server unreachable: " + server_url_);
    if (res->status != 200) {
      std::string res_body = sanitize(res->body);
      try {
        return json::parse(res_body);
      } catch (...) {
        return make_error("HTTP_" + std::to_string(res->status),
                          res_body.empty()
                              ? "Status " + std::to_string(res->status)
                              : res_body);
      }
    }
    return json::parse(sanitize(res->body));
  } catch (const std::exception &e) {
    return make_error("DELETE_ERROR", e.what());
  }
}

// ---------------------------------------------------------------------------
// Connection / Health
// ---------------------------------------------------------------------------

bool Database::initialize() {
  std::cout << "[Database] Connecting to: " << server_url_ << std::endl;
  json r = http_get("/health");
  if (r.value("success", false)) {
    std::cout << "[Database] ✓ Connected." << std::endl;
    return true;
  }
  last_error_ = r.value("error_message", "Health check failed");
  std::cout << "[Database] ✗ " << last_error_ << std::endl;
  return false;
}

bool Database::api_health_Check() {
  return http_get("/health").value("success", false);
}

void Database::setServerUrl(const std::string &new_url) {
  server_url_ = new_url;
  std::cout << "[Database] URL → " << new_url << std::endl;
}

// ---------------------------------------------------------------------------
// AUTH
// ---------------------------------------------------------------------------

// POST /api/auth/login
// Server expects plain-text password and hashes it server-side.
json Database::api_auth_Login(const std::string &username,
                              const std::string &password) {
  json r = http_post("/api/auth/login",
                     {{"username", username}, {"password", password}});
  std::cout << "[Auth] Login response: " << r.dump() << std::endl;
  return r;
}

// POST /api/auth/update_login
bool Database::api_auth_UpdateLogin(int user_id) {
  return http_post("/api/auth/update_login", {{"user_id", user_id}})
      .value("success", false);
}

// ---------------------------------------------------------------------------
// ADMIN — Users
// ---------------------------------------------------------------------------

json Database::api_admin_ListUsers() {
  json r = http_get("/api/admin/users");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/admin/users/create
// ADDED: capabilities parameter to ensure new users aren't born "powerless"
json Database::api_admin_CreateUser(const std::string &username,
                                    const std::string &password,
                                    const std::string &full_name,
                                    const std::string &role,
                                    const std::string &capabilities,
                                    int zone_id) {
  json body = {{"username", username},
               {"password", password},
               {"full_name", full_name},
               {"role", role},
               {"capabilities", capabilities}};
  if (zone_id > 0)
    body["id_zone"] = zone_id;

  json r = http_post("/api/admin/users/create", body);
  return r;
}

// POST /api/admin/users/update
bool Database::api_admin_UpdateUser(int user_id, const json &fields) {
  json body = fields;
  body["user_id"] = user_id;
  json r = http_post("/api/admin/users/update", body);
  return r.value("success", false);
}

// POST /api/admin/users/delete
bool Database::api_admin_DeleteUser(int user_id) {
  json r = http_post("/api/admin/users/delete", {{"user_id", user_id}});
  return r.value("success", false);
}

// ---------------------------------------------------------------------------
// ADMIN — Sessions & Logs
// ---------------------------------------------------------------------------

// GET /api/admin/sessions  — never cached (live data)
json Database::api_admin_ListSessions() {
  json r = http_get("/api/admin/sessions");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/admin/sessions/deactivate
bool Database::api_admin_DeactivateSession(int user_id) {
  return http_post("/api/admin/sessions/deactivate", {{"user_id", user_id}})
      .value("success", false);
}

// GET /api/admin/logs  — never cached
json Database::api_admin_GetLogs() {
  json r = http_get("/api/admin/logs");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// ---------------------------------------------------------------------------
// ZONES: Client-Side Implementation
// ---------------------------------------------------------------------------

// GET /api/zones -> Returns only active zones (synced with server WHERE active
// = TRUE)
json Database::api_zone_List() {
  json r = http_get("/api/zones");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/create_zone
json Database::api_zone_Create(const std::string &code,
                               const std::string &name) {
  // Simple creation; server defaults 'active' to true
  json r = http_post("/api/create_zone", {{"code", code}, {"name", name}});
  return r;
}

// PUT /api/zones/<id> -> Supports Renaming AND Deactivating
json Database::api_zone_Update(int id, const std::string &code,
                               const std::string &name, bool active) {
  // We now pass 'active' in the body.
  // If Flutter wants to "Delete" a zone, it calls this with active = false.
  json r = http_put("/api/zones/" + std::to_string(id),
                    {{"code", code}, {"name", name}, {"active", active}});
  return r;
}

// ---------------------------------------------------------------------------
// PRODUCTS
// ---------------------------------------------------------------------------

json Database::api_products_List() {
  json r = http_get("/api/complete_products");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/products/create
int Database::api_products_Create(const std::string &name) {
  json r = http_post("/api/products/create", {{"name", name}});
  if (r.value("success", false) && r.contains("data") &&
      r["data"].is_number()) {
    return r["data"].get<int>();
  }
  std::cout << "[Database] products/create failed: "
            << r.value("error_message", "unknown") << std::endl;
  return -1;
}

// POST /api/products/update
bool Database::api_products_Update(int id, const std::string &name) {
  json r = http_post("/api/products/update", {{"id", id}, {"name", name}});
  return r.value("success", false);
}

// POST /api/products/delete
bool Database::api_products_Delete(int id) {
  json r = http_post("/api/products/delete", {{"id", id}});
  return r.value("success", false);
}

// ---------------------------------------------------------------------------
// MATERIAL TYPES
// ---------------------------------------------------------------------------

json Database::api_types_List(const std::string &search) {
  json body = json::object();
  if (!search.empty())
    body["search"] = search;

  json r = http_post("/api/type/list", body);
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// GET /api/type/<id>  — tries cache first, falls back to server
json Database::api_types_Get(int id) {
  json r = http_get("/api/type/" + std::to_string(id));
  if (r.value("success", false) && r.contains("data") && r["data"].is_object())
    return r["data"];
  return json(nullptr);
}

// POST /api/type/create
int Database::api_types_Create(const json &data) {
  std::cout << "[Database] type/create payload: " << data.dump(2) << std::endl;
  json r = http_post("/api/type/create", data);
  std::cout << "[Database] type/create response: " << r.dump(2) << std::endl;
  if (r.value("success", false) && r.contains("data") &&
      r["data"].is_number()) {
    return r["data"].get<int>();
  }
  return -1;
}

// POST /api/type/update  — data must contain "id"
bool Database::api_types_Update(const json &data) {
  json r = http_post("/api/type/update", data);
  return r.value("success", false);
}

// POST /api/type/delete
bool Database::api_types_Delete(int id) {
  json r = http_post("/api/type/delete", {{"id", id}});
  return r.value("success", false);
}

// ---------------------------------------------------------------------------
// WEIGHINGS  — no cache
// ---------------------------------------------------------------------------

// POST /api/weigh/create
// Returns full response object with both id and qr_code
json Database::api_weigh_Create(const json &payload) {
  std::cout << "[Database] weigh/create payload: " << payload.dump(2)
            << std::endl;
  json r = http_post("/api/weigh/create", payload);
  std::cout << "[Database] weigh/create response: " << r.dump(2) << std::endl;
  if (r.value("success", false) && r.contains("data") && r["data"].is_object())
    return r["data"];
  std::cout << "[Database] weigh/create failed: "
            << r.value("error_message", "unknown") << std::endl;
  return json(nullptr);
}

// GET /api/weigh/details/<uuid>
json Database::api_weigh_GetByUUID(const std::string &uuid) {
  json r = http_get("/api/weigh/details/" + uuid);
  if (r.value("success", false) && r.contains("data") && r["data"].is_object())
    return r["data"];
  std::cout << "[Database] weigh/details not found: " << uuid << std::endl;
  return json(nullptr);
}

// POST /api/weigh/list
json Database::api_weigh_List(const json &filters) {
  json r = http_post("/api/weigh/list", filters);
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/my_weighings
json Database::api_weigh_MyList(int user_id) {
  json r = http_post("/api/my_weighings", {{"user_id", user_id}});
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/correct/weighing
bool Database::api_weigh_Correct(int weighing_id, int user_id,
                                 const std::string &reason,
                                 const json &changes) {
  json body = changes;
  body["id"] = weighing_id;
  body["user_id"] = user_id;
  body["reason"] = reason;
  json r = http_post("/api/correct/weighing", body);
  return r.value("success", false);
}

// ---------------------------------------------------------------------------
// RECONCILIATION  — no cache
// ---------------------------------------------------------------------------

// POST /api/reco/by_qr
json Database::api_reco_GetByQR(const std::string &qr_data) {
  std::cout << "[Database] reco/by_qr searching: " << qr_data << std::endl;
  json r = http_post("/api/reco/by_qr", {{"qr_code_data", qr_data}});
  if (r.value("success", false) && r.contains("data") &&
      r["data"].is_object()) {
    std::cout << "[Database] reco/by_qr found: " << r["data"].dump(2)
              << std::endl;
    return r["data"];
  }
  std::cout << "[Database] reco/by_qr not found: "
            << r.value("error_message", "no detail") << std::endl;
  return json(nullptr);
}

// POST /api/reco/submit
json Database::api_reco_Submit(int weighing_id, int user_id, double new_net,
                               double new_gross, double new_tare) {
  json body = {{"weighing_id", weighing_id},
               {"user_id", user_id},
               {"new_weight", new_net},
               {"new_gross", new_gross},
               {"new_tare", new_tare}};

  std::cout << "[Database] reco/submit payload: " << body.dump() << std::endl;
  json r = http_post("/api/reco/submit", body);
  std::cout << "[Database] reco/submit response: " << r.dump() << std::endl;

  if (r.value("success", false) && r.contains("data"))
    return r["data"];
  std::cout << "[Database] reco/submit failed: "
            << r.value("error_message", "unknown") << std::endl;
  return json(nullptr);
}

// POST /api/reco/accept
bool Database::api_reco_Accept(int reco_id, int user_id) {
  json r = http_post("/api/reco/accept",
                     {{"reco_id", reco_id}, {"user_id", user_id}});
  if (!r.value("success", false))
    std::cout << "[Database] reco/accept failed: "
              << r.value("error_message", "unknown") << std::endl;
  return r.value("success", false);
}

// POST /api/reco/reject
bool Database::api_reco_Reject(int reco_id, int user_id,
                               const std::string &reason) {
  json r = http_post(
      "/api/reco/reject",
      {{"reco_id", reco_id}, {"user_id", user_id}, {"reason", reason}});
  if (!r.value("success", false))
    std::cout << "[Database] reco/reject failed: "
              << r.value("error_message", "unknown") << std::endl;
  return r.value("success", false);
}

// POST /api/reco/list
json Database::api_reco_List(const json &filters) {
  json r = http_post("/api/reco/list", filters);
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// GET /api/reco/pending
json Database::api_reco_Pending() {
  json r = http_get("/api/reco/pending");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/my_Reconciliations
json Database::api_reco_MyList(int user_id, const std::string &status) {
  json r =
      http_post("/api/my_Reconciliations",
                {{"user_id", std::to_string(user_id)}, {"status", status}});
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/correct/reco
bool Database::api_reco_Correct(int reco_id, int user_id,
                                const std::string &reason,
                                const json &changes) {
  json body = changes;
  body["id"] = reco_id;
  body["user_id"] = user_id;
  body["reason"] = reason;
  json r = http_post("/api/correct/reco", body);
  return r.value("success", false);
}

// ---------------------------------------------------------------------------
// SHIPMENTS  — no cache
// ---------------------------------------------------------------------------

// POST /api/ship/by_qr
json Database::api_ship_GetByQR(const std::string &qr_data) {
  json r = http_post("/api/ship/by_qr", {{"qr_code_data", qr_data}});
  if (r.value("success", false) && r.contains("data") && r["data"].is_object())
    return r["data"];
  std::cout << "[Database] ship/by_qr: "
            << r.value("error_message", "not found") << std::endl;
  return json(nullptr);
}

// GET /api/ship/pending
json Database::api_ship_Pending() {
  json r = http_get("/api/ship/pending");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

bool Database::api_ship_CorrectStatus(int shipment_id, int user_id,
                                      const std::string &new_status,
                                      const std::string &reason) {
  // 1. Prepare the JSON payload
  nlohmann::json body = {{"shipment_id", shipment_id},
                         {"user_id", user_id},
                         {"new_status", new_status},
                         {"reason", reason}};

  // 2. Perform the POST request to the Crow server
  nlohmann::json response = http_post("/api/ship/correction", body);

  // 3. Log failure for debugging in the Linux terminal
  if (!response.value("success", false)) {
    std::cerr << "[Database] Shipment Correction Failed: "
              << response.value("error_message", "Unknown Server Error")
              << std::endl;
    return false;
  }

  return true;
}

// POST /api/ship/dispatch
json Database::api_ship_Dispatch(int shipment_id, int user_id,
                                 const std::string &status,
                                 const std::string &note) {
  json r = http_post("/api/ship/dispatch", {{"shipment_id", shipment_id},
                                            {"user_id", user_id},
                                            {"status", status},
                                            {"note", note}});
  if (!r.value("success", false)) {
    std::cout << "[Database] ship/dispatch failed: "
              << r.value("error_message", "unknown") << std::endl;
    return json(nullptr);
  }

  // Return the data object — contains shipment_id and flagged_warning
  return r.contains("data") ? r["data"] : json::object();
}

// POST /api/ship/list
json Database::api_ship_List(const json &filters) {
  json r = http_post("/api/ship/list", filters);
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/my_Shipments
json Database::api_ship_MyList(int user_id, const std::string &status) {
  json r = http_post("/api/my_Shipments",
                     {{"user_id", user_id}, {"status", status}});
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// ---------------------------------------------------------------------------
// DENATURATION  — no cache
// ---------------------------------------------------------------------------

// GET /api/denat/scan/<qr_code>
json Database::api_denat_ScanByQR(const std::string &qr_code) {
  json r = http_get("/api/denat/scan/" + qr_code);
  if (r.value("success", false) && r.contains("data") && r["data"].is_object())
    return r["data"];
  std::cout << "[Database] denat/scan not found: " << qr_code << std::endl;
  return json(nullptr);
}

// POST /api/denature/submit
bool Database::api_denat_Submit(int denat_id, int user_id, double brut_after,
                                double net_after, const std::string &qr_scanned,
                                const std::string &note) {
  json r = http_post(
      "/api/denature/submit",
      {{"denat_id", denat_id},
       {"user_id", user_id},
       {"weight_brut_after", brut_after},
       {"weight_net_after", net_after},
       {"qr_scanned", qr_scanned},
       {"note", note.empty() ? "Chemical stabilization complete" : note}});
  if (!r.value("success", false))
    std::cout << "[Database] denature/submit failed: "
              << r.value("error_message", "unknown") << std::endl;
  return r.value("success", false);
}

// POST /api/denature/correct
bool Database::api_denat_Correct(int denat_id, int user_id, double new_net,
                                 double new_brut, const std::string &reason) {
  json r = http_post("/api/denature/correct", {{"denat_id", denat_id},
                                               {"user_id", user_id},
                                               {"new_net_weight", new_net},
                                               {"new_brut_weight", new_brut},
                                               {"reason", reason}});
  return r.value("success", false);
}

// POST /api/denature/my_list
json Database::api_denat_MyList(int user_id, int limit) {
  json r = http_post("/api/denature/my_list",
                     {{"user_id", user_id}, {"limit", limit}});
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/denature/list_all
json Database::api_denat_ListAll(const std::string &status, int limit) {
  json r = http_post("/api/denature/list_all",
                     {{"status", status}, {"limit", limit}});
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// GET /api/denat/list_pending
// Targets the high-traceability queue for the Coordinator
json Database::api_denat_ListPending() {
  // Note: Since the backend route is GET /api/denature/list_pending
  json r = http_get("/api/denature/list_pending");

  if (r.value("success", false) && r.contains("data") && r["data"].is_array()) {
    return r["data"];
  }

  std::cout << "[Database] list_pending failed or empty" << std::endl;
  return json::array(); // Return empty array to keep Flutter loops safe
}

// ---------------------------------------------------------------------------
// FLAGS  — no cache
// ---------------------------------------------------------------------------

// GET /api/flags/list
json Database::api_flags_List() {
  json r = http_get("/api/flags/list");
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// POST /api/flags/mark_read
bool Database::api_flags_MarkRead(int flag_id) {
  json r = http_post("/api/flags/mark_read", {{"id", flag_id}});
  return r.value("success", false);
}

// ---------------------------------------------------------------------------
// CORRECTIONS  — no cache
// ---------------------------------------------------------------------------

// POST /api/correction/list
json Database::api_correction_List(const std::string &table_name,
                                   int record_id) {
  json body = json::object();
  if (!table_name.empty())
    body["table_name"] = table_name;
  if (record_id > 0)
    body["record_id"] = record_id;
  json r = http_post("/api/correction/list", body);
  return r.value("success", false) && r.contains("data") ? r["data"]
                                                         : json::array();
}

// ---------------------------------------------------------------------------
// EXPORT  — no cache (can be large and slow, caller decides when to call)
// ---------------------------------------------------------------------------

// GET /api/export/all
json Database::api_export_All() {
  json r = http_get("/api/export/all");
  // Server returns { "data": [...], "count": N } directly (not wrapped in
  // success)
  if (r.contains("data") && r["data"].is_array())
    return r["data"];
  return json::array();
}