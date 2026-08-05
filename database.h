#ifndef DATABASE_H
#define DATABASE_H

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

class Database {
public:
  explicit Database(const std::string &server_url);
  ~Database() = default;

  bool initialize();
  void setServerUrl(const std::string &new_url);

  // =======================================================================
  // AUTH
  //   POST /api/auth/login
  //   POST /api/auth/update_login
  // =======================================================================
  json api_auth_Login(const std::string &username, const std::string &password);
  bool api_auth_UpdateLogin(int user_id);

  // =======================================================================
  // ADMIN — Users
  //   GET  /api/admin/users
  //   POST /api/admin/users/create
  //   POST /api/admin/users/update
  //   POST /api/admin/users/delete
  // =======================================================================
  json api_admin_ListUsers();
  json api_admin_CreateUser(const std::string &username,
                            const std::string &password,
                            const std::string &full_name,
                            const std::string &role,
                            const std::string &capabilities, int zone_id = -1);
  bool api_admin_UpdateUser(int user_id, const json &fields);
  bool api_admin_DeleteUser(int user_id);

  // =======================================================================
  // ADMIN — Sessions & Logs
  //   GET  /api/admin/sessions
  //   POST /api/admin/sessions/deactivate
  //   GET  /api/admin/logs
  // =======================================================================
  json api_admin_ListSessions();
  bool api_admin_DeactivateSession(int user_id);
  json api_admin_GetLogs();

  // =======================================================================
  // ZONES
  //   GET    /api/zones
  //   POST   /api/create_zone
  //   PUT    /api/zones/<id>
  //   DELETE /api/zones/<id>
  // =======================================================================
  json api_zone_List();
  json api_zone_Create(const std::string &code, const std::string &name);
  json api_zone_Update(int id, const std::string &code, const std::string &name,
                       bool active = true);

  // =======================================================================
  // PRODUCTS
  //   GET  /api/complete_products
  //   POST /api/products/create
  //   POST /api/products/update
  //   POST /api/products/delete
  // =======================================================================
  json api_products_List();
  int api_products_Create(const std::string &name);
  bool api_products_Update(int id, const std::string &name);
  bool api_products_Delete(int id);

  // =======================================================================
  // MATERIAL TYPES
  //   POST /api/type/list
  //   GET  /api/type/<id>
  //   POST /api/type/create
  //   POST /api/type/update
  //   POST /api/type/delete
  // =======================================================================
  json api_types_List(const std::string &search = "");
  json api_types_Get(int id);
  int api_types_Create(const json &data);
  bool api_types_Update(const json &data); // must contain "id"
  bool api_types_Delete(int id);

  // =======================================================================
  // WEIGHINGS
  //   POST /api/weigh/create
  //   GET  /api/weigh/details/<uuid>
  //   POST /api/weigh/list
  //   POST /api/my_weighings
  //   POST /api/correct/weighing
  // =======================================================================
  json api_weigh_Create(const json &payload);
  json api_weigh_GetByUUID(const std::string &uuid);
  json api_weigh_List(const json &filters = json::object());
  json api_weigh_MyList(int user_id);
  bool api_weigh_Correct(int weighing_id, int user_id,
                         const std::string &reason, const json &changes);

  // =======================================================================
  // RECONCILIATION
  //   POST /api/reco/by_qr
  //   POST /api/reco/submit
  //   POST /api/reco/accept
  //   POST /api/reco/reject
  //   POST /api/reco/list
  //   GET  /api/reco/pending
  //   POST /api/my_Reconciliations
  //   POST /api/correct/reco
  // =======================================================================
  json api_reco_GetByQR(const std::string &qr_data);
  json api_reco_Submit(int weighing_id, int user_id, double new_net,
                       double new_gross, double new_tare);
  bool api_reco_Accept(int reco_id, int user_id);
  bool api_reco_Reject(int reco_id, int user_id, const std::string &reason);
  json api_reco_List(const json &filters = json::object());
  json api_reco_Pending();
  json api_reco_MyList(int user_id, const std::string &status = "all");
  bool api_reco_Correct(int reco_id, int user_id, const std::string &reason,
                        const json &changes);

  // =======================================================================
  // SHIPMENTS
  //   POST /api/ship/by_qr
  //   GET  /api/ship/pending
  //   POST /api/ship/dispatch
  //   POST /api/ship/list
  //   POST /api/my_Shipments
  // =======================================================================
  json api_ship_GetByQR(const std::string &qr_data);
  json api_ship_Pending();
  json api_ship_Dispatch(int shipment_id, int user_id,
                         const std::string &status = "shipped",
                         const std::string &note = "Loaded onto truck");
  json api_ship_List(const json &filters = json::object());
  json api_ship_MyList(int user_id, const std::string &status = "all");
  /**
   * @brief Corrects the status of a shipment (e.g., reverting 'shipped' to
   * 'pending').
   * @param shipment_id The ID of the record in the shipments table.
   * @param user_id The ID of the operator/supervisor making the fix.
   * @param new_status The target status: "pending", "shipped", or "cancelled".
   * @param reason Human-readable explanation for the audit log.
   * @return true if the status was updated and logged in the corrections table.
   */
  bool api_ship_CorrectStatus(int shipment_id, int user_id,
                              const std::string &new_status,
                              const std::string &reason);
  // =======================================================================
  // DENATURATION
  // GET  /api/denature/list_pending
  //   GET  /api/denat/scan/<qr_code>
  //   POST /api/denature/submit
  //   POST /api/denature/correct
  //   POST /api/denature/my_list
  //   POST /api/denature/list_all
  // =======================================================================
  // The "Active Frontline" Queue for Coordinators
  json api_denat_ListPending();
  json api_denat_ScanByQR(const std::string &qr_code);
  bool api_denat_Submit(int denat_id, int user_id, double brut_after,
                        double net_after, const std::string &qr_scanned,
                        const std::string &note = "");
  bool api_denat_Correct(int denat_id, int user_id, double new_net,
                         double new_brut, const std::string &reason);
  json api_denat_MyList(int user_id, int limit = 20);
  json api_denat_ListAll(const std::string &status = "all", int limit = 100);

  // =======================================================================
  // FLAGS
  //   GET  /api/flags/list
  //   POST /api/flags/mark_read
  // =======================================================================
  json api_flags_List();
  bool api_flags_MarkRead(int flag_id);

  // =======================================================================
  // CORRECTIONS
  //   POST /api/correction/list
  // =======================================================================
  json api_correction_List(const std::string &table_name = "",
                           int record_id = -1);

  // =======================================================================
  // EXPORT
  //   GET /api/export/all
  // =======================================================================
  json api_export_All();

  // =======================================================================
  // HEALTH
  //   GET /health
  // =======================================================================
  bool api_health_Check();

private:
  std::string server_url_;
  std::string last_error_;

  // HTTP primitives
  json http_get(const std::string &endpoint);
  json http_post(const std::string &endpoint, const json &body);
  json http_put(const std::string &endpoint, const json &body);
  json http_delete(const std::string &endpoint);

  static std::string sanitize(const std::string &raw);
  static json make_error(const std::string &code, const std::string &msg);
};

#endif // DATABASE_H