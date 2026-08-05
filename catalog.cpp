#include "catalog.h"
#include "auth.h"
#include "context.h"
#include <cstring>
#include <iostream>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Heap-allocates a C string copy for FFI transfer.
// Dart receives the pointer and MUST free it with context_free_string().
// Returns nullptr if the input string is empty.
static char *catalog_alloc(const std::string &s) {
  if (s.empty())
    return nullptr;
  char *p = static_cast<char *>(std::malloc(s.length() + 1));
  if (p)
    std::memcpy(p, s.c_str(), s.length() + 1);
  return p;
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

// Permission gate for write operations.
// Returns true only if the session is alive AND the role is ADMIN.
// Any other outcome — null handle, dead session, wrong role — returns false.
static bool catalog_write_allowed(WasteTrackingHandle handle,
                                  UserHandle user_handle) {
  if (!handle || !user_handle)
    return false;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return false;
  const char *result = ctx->auth->checkPermission(user_handle, "manage_users");
  return (std::strcmp(result, "OK") == 0);
}

// Maps the permission check result to the correct WasteTrackingError.
// Used when we need to return a specific error code instead of just false.
static WasteTrackingError permission_error(WasteTrackingHandle handle,
                                           UserHandle user_handle) {
  if (!handle || !user_handle)
    return WT_ERROR_INVALID_PARAM;
  auto *ctx = static_cast<WasteTrackingContext *>(handle);
  if (!ctx->auth)
    return WT_ERROR_INVALID_STATE;
  const char *result = ctx->auth->checkPermission(user_handle, "manage_users");
  if (std::strcmp(result, "NO_SESSION") == 0)
    return WT_ERROR_INVALID_PARAM;
  if (std::strcmp(result, "SESSION_DEAD") == 0)
    return WT_ERROR_INVALID_STATE;
  if (std::strcmp(result, "ROLE_DENIED") == 0)
    return WT_ERROR_PERMISSION;
  return WT_ERROR_PERMISSION; // safe default
}

// ============================================================================
// MATERIAL TYPES — FFI IMPLEMENTATION
// ============================================================================

extern "C" {

// ---------------------------------------------------------------------------
// type_create
// All 9 fields required at creation time.
// Builds the JSON payload and calls api_types_Create().
// Returns new id on success, -1 on any failure.
// ---------------------------------------------------------------------------
int32_t type_create(WasteTrackingHandle handle, UserHandle user_handle,
                    const char *code, const char *name, const char *nature,
                    const char *forme, int32_t tag_code, bool requires_lot,
                    bool requires_name, bool requires_product,
                    bool requires_denaturation) {

  if (!catalog_write_allowed(handle, user_handle)) {
    std::cerr << "[Catalog] type_create denied — permission check failed."
              << std::endl;
    return -1;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return -1;

  // Validate mandatory string fields
  if (!code || std::strlen(code) == 0) {
    std::cerr << "[Catalog] type_create — code is required." << std::endl;
    return -1;
  }
  if (!name || std::strlen(name) == 0) {
    std::cerr << "[Catalog] type_create — name is required." << std::endl;
    return -1;
  }

  // Build the JSON payload exactly as the server expects it
  json data;
  data["code"] = std::string(code);
  data["name"] = std::string(name);
  data["nature"] = nature ? std::string(nature) : "";
  data["forme"] = forme ? std::string(forme) : "";
  data["tag_code"] = tag_code;
  data["requires_lot"] = requires_lot;
  data["requires_name"] = requires_name;
  data["requires_product"] = requires_product;
  data["requires_denaturation"] = requires_denaturation;

  int32_t new_id = ctx->db->api_types_Create(data);

  if (new_id == -1)
    std::cerr << "[Catalog] type_create failed for code='" << code << "'"
              << std::endl;
  else
    std::cout << "[Catalog] type_create OK — id=" << new_id << " code='" << code
              << "'" << std::endl;

  return new_id;
}

// ---------------------------------------------------------------------------
// type_update
// Partial update — fields_json contains only the fields to change plus "id".
// The "id" inside fields_json is what the server uses to locate the record.
// type_id parameter is used for validation only — must match the "id" in JSON.
// ---------------------------------------------------------------------------
WasteTrackingError type_update(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t type_id,
                               const char *fields_json) {

  if (!catalog_write_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (type_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  if (!fields_json || std::strlen(fields_json) == 0)
    return WT_ERROR_INVALID_PARAM;

  // Parse and validate the fields JSON
  json fields;
  try {
    fields = json::parse(std::string(fields_json));
  } catch (...) {
    std::cerr << "[Catalog] type_update — invalid JSON in fields_json."
              << std::endl;
    return WT_ERROR_INVALID_PARAM;
  }

  // Inject the id so database.cpp can forward it correctly
  // If Dart already included "id", this overwrites it with the validated one
  fields["id"] = type_id;

  bool ok = ctx->db->api_types_Update(fields);

  if (!ok) {
    std::cerr << "[Catalog] type_update failed for id=" << type_id << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Catalog] type_update OK — id=" << type_id << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// type_delete
// Hard delete. Server blocks with a foreign key error if weighings exist.
// In that case return WT_ERROR_DATABASE — Dart should tell the user to
// rename instead of delete to preserve the audit trail.
// ---------------------------------------------------------------------------
WasteTrackingError type_delete(WasteTrackingHandle handle,
                               UserHandle user_handle, int32_t type_id) {

  if (!catalog_write_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (type_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  bool ok = ctx->db->api_types_Delete(type_id);

  if (!ok) {
    std::cerr << "[Catalog] type_delete failed for id=" << type_id
              << " — may have existing weighings (audit trail protected)."
              << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Catalog] type_delete OK — id=" << type_id << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// type_list
// Open read — no user_handle needed.
// search_term: pass nullptr or empty string for the full list.
// Returns heap-allocated JSON array string. Dart frees with
// context_free_string(). Returns nullptr if context is invalid or server is
// unreachable.
// ---------------------------------------------------------------------------
char *type_list(WasteTrackingHandle handle, const char *search_term) {
  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  std::string search = (search_term && std::strlen(search_term) > 0)
                           ? std::string(search_term)
                           : "";

  json list = ctx->db->api_types_List(search);

  if (!list.is_array()) {
    std::cerr << "[Catalog] type_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return catalog_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// type_get
// Open read — no user_handle needed.
// Returns heap-allocated JSON object string, or nullptr if not found.
// ---------------------------------------------------------------------------
char *type_get(WasteTrackingHandle handle, int32_t type_id) {
  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  if (type_id <= 0)
    return nullptr;

  json item = ctx->db->api_types_Get(type_id);

  if (item.is_null()) {
    std::cerr << "[Catalog] type_get — id=" << type_id << " not found."
              << std::endl;
    return nullptr;
  }

  return catalog_alloc(item.dump());
}

// ============================================================================
// BRAND PRODUCTS — FFI IMPLEMENTATION
// ============================================================================

// ---------------------------------------------------------------------------
// product_create
// name must be unique — server enforces ON CONFLICT.
// Returns new id on success, -1 on failure (e.g. duplicate name).
// ---------------------------------------------------------------------------
int32_t product_create(WasteTrackingHandle handle, UserHandle user_handle,
                       const char *name) {

  if (!catalog_write_allowed(handle, user_handle)) {
    std::cerr << "[Catalog] product_create denied — permission check failed."
              << std::endl;
    return -1;
  }

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return -1;

  if (!name || std::strlen(name) == 0) {
    std::cerr << "[Catalog] product_create — name is required." << std::endl;
    return -1;
  }

  int32_t new_id = ctx->db->api_products_Create(std::string(name));

  if (new_id == -1)
    std::cerr << "[Catalog] product_create failed — name='" << name
              << "' may already exist." << std::endl;
  else
    std::cout << "[Catalog] product_create OK — id=" << new_id << " name='"
              << name << "'" << std::endl;

  return new_id;
}

// ---------------------------------------------------------------------------
// product_update
// Replaces the product name. name must still be unique after update.
// ---------------------------------------------------------------------------
WasteTrackingError product_update(WasteTrackingHandle handle,
                                  UserHandle user_handle, int32_t product_id,
                                  const char *name) {

  if (!catalog_write_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (product_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  if (!name || std::strlen(name) == 0)
    return WT_ERROR_INVALID_PARAM;

  bool ok = ctx->db->api_products_Update(product_id, std::string(name));

  if (!ok) {
    std::cerr << "[Catalog] product_update failed for id=" << product_id
              << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Catalog] product_update OK — id=" << product_id
            << " new name='" << name << "'" << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// product_delete
// Hard delete. Server blocks if weighings reference this product.
// ---------------------------------------------------------------------------
WasteTrackingError product_delete(WasteTrackingHandle handle,
                                  UserHandle user_handle, int32_t product_id) {

  if (!catalog_write_allowed(handle, user_handle))
    return permission_error(handle, user_handle);

  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return WT_ERROR_INVALID_STATE;

  if (product_id <= 0)
    return WT_ERROR_INVALID_PARAM;

  bool ok = ctx->db->api_products_Delete(product_id);

  if (!ok) {
    std::cerr << "[Catalog] product_delete failed for id=" << product_id
              << " — may be referenced by existing weighings." << std::endl;
    return WT_ERROR_DATABASE;
  }

  std::cout << "[Catalog] product_delete OK — id=" << product_id << std::endl;
  return WT_SUCCESS;
}

// ---------------------------------------------------------------------------
// product_list
// Open read — no user_handle needed.
// Returns heap-allocated JSON array string. Dart frees with
// context_free_string(). Returns nullptr if context is invalid or server is
// unreachable.
// ---------------------------------------------------------------------------
char *product_list(WasteTrackingHandle handle) {
  auto *ctx = resolve_ctx(handle);
  if (!ctx)
    return nullptr;

  json list = ctx->db->api_products_List();

  if (!list.is_array()) {
    std::cerr << "[Catalog] product_list — unexpected response from server."
              << std::endl;
    return nullptr;
  }

  return catalog_alloc(list.dump());
}

// ---------------------------------------------------------------------------
// catalog_free_string
// ---------------------------------------------------------------------------
void catalog_free_string(char *ptr) {
  if (ptr)
    std::free(ptr);
}

// ============================================================================
// NOTIFICATION — FFI IMPLEMENTATION
// ============================================================================

// ---------------------------------------------------------------------------
// catalog_set_notify_callback
// Dart calls this once after login, before the socket connects.
// Stores the callback pointer on UserSession — survives socket rebuilds.
// Dart MUST use NativeCallable.permanent() to prevent GC collection.
// Call NativeCallable.close() on logout to release it.
// ---------------------------------------------------------------------------
void catalog_set_notify_callback(UserHandle user_handle,
                                 CatalogNotifyCallback cb) {
  if (!user_handle)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  if (!s->is_valid) {
    std::cerr << "[Catalog] catalog_set_notify_callback — session is dead."
              << std::endl;
    return;
  }

  s->catalog_notify_cb = cb;
  std::cout << "[Catalog] Notify callback registered for user " << s->user_id
            << std::endl;
}

// ---------------------------------------------------------------------------
// notify_catalog
// Called by the socket chamber when a catalog-related signal arrives.
// Fires Dart's callback with the signal name as a static string literal.
// Dart must NOT free signal_name — it is never heap allocated.
// Safe to call from any thread.
//
// Dart matches signal_name:
//   "TYPE_CREATED"     → calls type_list()
//   "TYPE_UPDATED"     → calls type_list()
//   "TYPE_DELETED"     → calls type_list()
//   "REFRESH_PRODUCTS" → calls product_list()
// ---------------------------------------------------------------------------
void notify_catalog(UserHandle user_handle, const char *signal_name) {
  if (!user_handle || !signal_name)
    return;

  auto *s = static_cast<UserSession *>(user_handle);

  // Session must be alive and not mid-logout
  // Protects against late-arriving signals after logout has started
  if (!s->is_valid || s->logout_in_progress) {
    std::cerr << "[Catalog] notify_catalog — session dead, dropping: "
              << signal_name << std::endl;
    return;
  }

  if (!s->catalog_notify_cb) {
    std::cerr << "[Catalog] notify_catalog — no callback registered, "
              << "dropping: " << signal_name << std::endl;
    return;
  }

  std::cout << "[Catalog] → Dart: " << signal_name << std::endl;
  s->catalog_notify_cb(signal_name);
}

} // extern "C"