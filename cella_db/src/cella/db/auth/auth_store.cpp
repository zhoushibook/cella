#include "cella/db/auth/auth_store.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <utility>

#include "cella/db/auth/password.h"
#include "cella/db/common/db_logger.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/record.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/status.h"
#include "cella/storage/common/value.h"
#include "cella/storage/table/table_heap.h"

namespace cella::db {
namespace {

DbStatus AuthStorageError(const char* what, const storage::Status& s) {
  return DbStatus::Error(DbCode::kStorageError,
                         std::string(what) + " [存储: " + s.ToString() + "]");
}

const storage::Value* ValueAt(const storage::Record& rec, size_t i) {
  return i < rec.value_count() ? &rec.value(i) : nullptr;
}

// 授权行的键（用户 + 库 + 表 + 权限；全部不区分大小写）
bool SameGrantKey(const AuthGrant& a, const AuthGrant& b) {
  return AuthStore::CanonicalName(a.user) == AuthStore::CanonicalName(b.user) &&
         AuthStore::CanonicalName(a.scope_db) == AuthStore::CanonicalName(b.scope_db) &&
         AuthStore::CanonicalName(a.scope_table) == AuthStore::CanonicalName(b.scope_table) &&
         a.priv == b.priv;
}

std::string ScopeText(const AuthGrant& g) {
  const std::string db = g.scope_db.empty() ? "*" : g.scope_db;
  const std::string tb = g.scope_table.empty() ? "*" : g.scope_table;
  return db + "." + tb;
}

}  // namespace

std::string AuthStore::CanonicalName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

bool AuthStore::ValidUserName(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameLen) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(name[0]);
  if (std::isalpha(first) == 0 && name[0] != '_') {
    return false;
  }
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) == 0 && c != '_') {
      return false;
    }
  }
  return true;
}

// ── schema ──────────────────────────────────────────────────

storage::Schema AuthStore::UsersSchema() {
  storage::Schema s;
  s.AddColumn("name", storage::ValueType::kVarchar, 64);
  s.AddColumn("pwd", storage::ValueType::kVarchar, 0);  // 0 = 不限长（哈希串 ~110 字节）
  s.AddColumn("is_admin", storage::ValueType::kInt32, 0);
  s.AddColumn("created_at", storage::ValueType::kInt32, 0);
  return s;
}

storage::Schema AuthStore::PrivilegesSchema() {
  storage::Schema s;
  s.AddColumn("user", storage::ValueType::kVarchar, 64);
  s.AddColumn("scope_db", storage::ValueType::kVarchar, 64);
  s.AddColumn("scope_table", storage::ValueType::kVarchar, 64);
  s.AddColumn("priv", storage::ValueType::kVarchar, 16);
  s.AddColumn("grantable", storage::ValueType::kInt32, 0);
  return s;
}

// ── 生命周期 ────────────────────────────────────────────────

DbStatus AuthStore::EnsureTables(bool* created) {
  if (created != nullptr) {
    *created = false;
  }
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  struct Spec {
    const char* name;
    storage::Schema (*schema)();
  };
  const Spec specs[] = {{kUsersTable, &AuthStore::UsersSchema},
                        {kPrivilegesTable, &AuthStore::PrivilegesSchema}};
  for (const Spec& spec : specs) {
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status os = storage_->open_table(spec.name, &heap);
    if (os.ok()) {
      continue;
    }
    if (os.code() != storage::StatusCode::kTableNotFound) {
      return DbStatus::Error(DbCode::kStorageError,
                             std::string("打开身份表失败: ") + spec.name + " " + os.ToString());
    }
    const storage::Status cs = storage_->create_table(spec.name, spec.schema());
    if (!cs.ok()) {
      return DbStatus::Error(DbCode::kStorageError,
                             std::string("创建身份表失败: ") + spec.name + " " + cs.ToString());
    }
    if (created != nullptr) {
      *created = true;
    }
    DbLogInfo(logcat::kAuth, std::string("已创建身份表 ") + spec.name);
  }
  return DbStatus::Ok();
}

DbStatus AuthStore::LoadFromStorage() {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  std::map<std::string, AuthUser> loaded;

  std::shared_ptr<storage::TableHeap> heap;
  const storage::Status os = storage_->open_table(kUsersTable, &heap);
  if (!os.ok()) {
    return DbStatus::Error(DbCode::kStorageError,
                           "打开用户表失败: " + os.ToString());
  }
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    const storage::Record& rec = *it;
    const storage::Value* name = ValueAt(rec, 0);
    const storage::Value* pwd = ValueAt(rec, 1);
    const storage::Value* adm = ValueAt(rec, 2);
    const storage::Value* cat = ValueAt(rec, 3);
    if (name == nullptr || pwd == nullptr || adm == nullptr || cat == nullptr ||
        name->type != storage::ValueType::kVarchar || pwd->type != storage::ValueType::kVarchar ||
        adm->type != storage::ValueType::kInt32 || cat->type != storage::ValueType::kInt32) {
      DbLogWarn(logcat::kAuth, "用户表中存在无法解析的行，已跳过");
      continue;
    }
    AuthUser u;
    u.name = name->str_val;
    u.pwd = pwd->str_val;
    u.is_admin = (adm->int32_val != 0);
    u.created_at = static_cast<int64_t>(cat->int32_val);
    const std::string key = CanonicalName(u.name);
    if (loaded.count(key) != 0) {
      DbLogWarn(logcat::kAuth, "用户表中存在重复用户名，已跳过: " + u.name);
      continue;
    }
    loaded[key] = std::move(u);
  }

  // ── 权限行 ──
  std::vector<AuthGrant> loaded_grants;
  std::shared_ptr<storage::TableHeap> pheap;
  const storage::Status pos = storage_->open_table(kPrivilegesTable, &pheap);
  if (!pos.ok()) {
    return DbStatus::Error(DbCode::kStorageError, "打开权限表失败: " + pos.ToString());
  }
  for (auto it = pheap->begin(); it != pheap->end(); ++it) {
    const storage::Record& rec = *it;
    const storage::Value* u = ValueAt(rec, 0);
    const storage::Value* db = ValueAt(rec, 1);
    const storage::Value* tbl = ValueAt(rec, 2);
    const storage::Value* pv = ValueAt(rec, 3);
    const storage::Value* gr = ValueAt(rec, 4);
    if (u == nullptr || db == nullptr || tbl == nullptr || pv == nullptr || gr == nullptr ||
        u->type != storage::ValueType::kVarchar ||
        db->type != storage::ValueType::kVarchar ||
        tbl->type != storage::ValueType::kVarchar ||
        pv->type != storage::ValueType::kVarchar || gr->type != storage::ValueType::kInt32) {
      DbLogWarn(logcat::kAuth, "权限表中存在无法解析的行，已跳过");
      continue;
    }
    AuthGrant g;
    g.user = u->str_val;
    g.scope_db = db->str_val;
    g.scope_table = tbl->str_val;
    if (!ParsePriv(pv->str_val, &g.priv)) {
      DbLogWarn(logcat::kAuth, "权限表中存在未知权限名，已跳过: " + pv->str_val);
      continue;
    }
    g.grantable = (gr->int32_val != 0);
    loaded_grants.push_back(std::move(g));
  }

  size_t n_users = 0;
  size_t n_grants = 0;
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    users_ = std::move(loaded);
    grants_ = std::move(loaded_grants);
    n_users = users_.size();
    n_grants = grants_.size();
  }
  DbLogInfo(logcat::kAuth, "身份库已加载: " + std::to_string(n_users) + " 个用户 / " +
                               std::to_string(n_grants) + " 条授权");
  return DbStatus::Ok();
}

// ── 行读写 ──────────────────────────────────────────────────

storage::Rid AuthStore::FindUserRow(const std::string& name) const {
  storage::Rid none;
  if (storage_ == nullptr) {
    return none;
  }
  std::shared_ptr<storage::TableHeap> heap;
  if (!storage_->open_table(kUsersTable, &heap).ok()) {
    return none;
  }
  const std::string key = CanonicalName(name);
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    const storage::Record& rec = *it;
    if (rec.value_count() > 0 && rec.value(0).type == storage::ValueType::kVarchar &&
        CanonicalName(rec.value(0).str_val) == key) {
      return it.rid();
    }
  }
  return none;
}

DbStatus AuthStore::InsertUserRow(const AuthUser& user) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  storage::Record rec;
  rec.AddValue(storage::Value::Varchar(user.name));
  rec.AddValue(storage::Value::Varchar(user.pwd));
  rec.AddValue(storage::Value::Int(user.is_admin ? 1 : 0));
  rec.AddValue(storage::Value::Int(static_cast<int32_t>(user.created_at)));
  storage::Rid rid;
  const storage::Status s = storage_->insert_record(kUsersTable, rec, &rid);
  if (!s.ok()) {
    return AuthStorageError("写用户表失败", s);
  }
  return DbStatus::Ok();
}

DbStatus AuthStore::UpdateUserRow(const AuthUser& user) {
  const storage::Rid rid = FindUserRow(user.name);
  if (!rid.IsValid()) {
    return DbStatus::Error(DbCode::kUserError, "用户表中未找到用户行: " + user.name);
  }
  const storage::Status ds = storage_->delete_record(kUsersTable, rid);
  if (!ds.ok()) {
    return AuthStorageError("更新用户（删旧行）失败", ds);
  }
  return InsertUserRow(user);
}

DbStatus AuthStore::DeleteUserRow(const std::string& name) {
  const storage::Rid rid = FindUserRow(name);
  if (!rid.IsValid()) {
    return DbStatus::Error(DbCode::kUserError, "用户表中未找到用户行: " + name);
  }
  const storage::Status s = storage_->delete_record(kUsersTable, rid);
  if (!s.ok()) {
    return AuthStorageError("删用户行失败", s);
  }
  return DbStatus::Ok();
}

// ── 用户管理 ────────────────────────────────────────────────

DbStatus AuthStore::CreateUser(const std::string& name, const std::string& password, bool is_admin,
                               std::string* note) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  if (!ValidUserName(name)) {
    return DbStatus::Error(DbCode::kUserError,
                           "用户名非法: " + name + "（1..64 个字母/数字/下划线，且以字母或下划线开头）");
  }
  const std::string key = CanonicalName(name);
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (users_.count(key) != 0) {
      return DbStatus::Error(DbCode::kUserError, "用户已存在: " + name);
    }
  }
  AuthUser u;
  u.name = name;
  u.pwd = HashPassword(password);
  u.is_admin = is_admin;
  u.created_at = static_cast<int64_t>(std::time(nullptr));
  const DbStatus ws = InsertUserRow(u);
  if (!ws.ok()) {
    return ws;
  }
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    users_[key] = u;
  }
  if (note != nullptr) {
    *note = "用户 " + u.name + " 已创建" + (is_admin ? "（管理员）" : "");
  }
  DbLogInfo(logcat::kAuth, "创建用户 " + u.name + (is_admin ? "（管理员）" : ""));
  return DbStatus::Ok();
}

DbStatus AuthStore::DropUser(const std::string& name, std::string* note) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  const std::string key = CanonicalName(name);
  bool is_admin = false;
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    const auto it = users_.find(key);
    if (it == users_.end()) {
      return DbStatus::Error(DbCode::kUserError, "用户不存在: " + name);
    }
    is_admin = it->second.is_admin;
    if (is_admin && admin_count() <= 1) {
      return DbStatus::Error(DbCode::kLastAdmin,
                             "不能删除最后一个管理员: " + it->second.name);
    }
  }
  const DbStatus ds = DeleteUserRow(name);
  if (!ds.ok()) {
    return ds;
  }
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    users_.erase(key);
  }
  if (note != nullptr) {
    *note = "用户 " + name + " 已删除";
  }
  DbLogInfo(logcat::kAuth, "删除用户 " + name);
  return DbStatus::Ok();
}

DbStatus AuthStore::SetPassword(const std::string& name, const std::string& password,
                                std::string* note) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  const std::string key = CanonicalName(name);
  AuthUser updated;
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    const auto it = users_.find(key);
    if (it == users_.end()) {
      return DbStatus::Error(DbCode::kUserError, "用户不存在: " + name);
    }
    updated = it->second;
  }
  updated.pwd = HashPassword(password);
  const DbStatus ws = UpdateUserRow(updated);
  if (!ws.ok()) {
    return ws;
  }
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    users_[key] = updated;
  }
  if (note != nullptr) {
    *note = "用户 " + updated.name + " 的口令已更新";
  }
  DbLogInfo(logcat::kAuth, "更新口令: " + updated.name);
  return DbStatus::Ok();
}

// ── 权限 ────────────────────────────────────────────────────

storage::Rid AuthStore::FindGrantRow(const AuthGrant& grant) const {
  storage::Rid none;
  if (storage_ == nullptr) {
    return none;
  }
  std::shared_ptr<storage::TableHeap> heap;
  if (!storage_->open_table(kPrivilegesTable, &heap).ok()) {
    return none;
  }
  const std::string u = CanonicalName(grant.user);
  const std::string db = CanonicalName(grant.scope_db);
  const std::string tb = CanonicalName(grant.scope_table);
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    const storage::Record& rec = *it;
    if (rec.value_count() < 4) {
      continue;
    }
    if (CanonicalName(rec.value(0).str_val) == u && CanonicalName(rec.value(1).str_val) == db &&
        CanonicalName(rec.value(2).str_val) == tb &&
        CanonicalName(rec.value(3).str_val) == CanonicalName(PrivName(grant.priv))) {
      return it.rid();
    }
  }
  return none;
}

DbStatus AuthStore::InsertGrantRow(const AuthGrant& grant) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  storage::Record rec;
  rec.AddValue(storage::Value::Varchar(grant.user));
  rec.AddValue(storage::Value::Varchar(grant.scope_db));
  rec.AddValue(storage::Value::Varchar(grant.scope_table));
  rec.AddValue(storage::Value::Varchar(PrivName(grant.priv)));
  rec.AddValue(storage::Value::Int(grant.grantable ? 1 : 0));
  storage::Rid rid;
  const storage::Status s = storage_->insert_record(kPrivilegesTable, rec, &rid);
  if (!s.ok()) {
    return AuthStorageError("写权限表失败", s);
  }
  return DbStatus::Ok();
}

DbStatus AuthStore::DeleteGrantRow(const storage::Rid& rid) {
  const storage::Status s = storage_->delete_record(kPrivilegesTable, rid);
  if (!s.ok()) {
    return AuthStorageError("删权限行失败", s);
  }
  return DbStatus::Ok();
}

// 把用户拼写统一成已登记的写法（避免同一个人出现多种拼写）
DbStatus AuthStore::ResolveUserName(const std::string& user, std::string* out) const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const auto it = users_.find(CanonicalName(user));
  if (it == users_.end()) {
    return DbStatus::Error(DbCode::kUserError, "用户不存在: " + user);
  }
  *out = it->second.name;
  return DbStatus::Ok();
}

DbStatus AuthStore::GrantPrivilege(const std::string& user, const std::string& scope_db,
                                   const std::string& scope_table, Priv priv, bool grantable,
                                   std::string* note) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  if (priv == Priv::kAdmin) {
    return DbStatus::Error(DbCode::kInternal, "ADMIN 由 SetAdmin 处理，不应走权限行");
  }
  AuthGrant g;
  const DbStatus rs = ResolveUserName(user, &g.user);
  if (!rs.ok()) {
    return rs;
  }
  g.scope_db = scope_db;
  g.scope_table = scope_table;
  g.priv = priv;
  g.grantable = grantable;

  const storage::Rid old = FindGrantRow(g);
  if (old.IsValid()) {
    const DbStatus ds = DeleteGrantRow(old);
    if (!ds.ok()) {
      return ds;
    }
  }
  const DbStatus ws = InsertGrantRow(g);
  if (!ws.ok()) {
    return ws;
  }
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    bool replaced = false;
    for (AuthGrant& e : grants_) {
      if (SameGrantKey(e, g)) {
        e = g;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      grants_.push_back(g);
    }
  }
  if (note != nullptr) {
    *note = "已授予 " + g.user + " " + PrivName(g.priv) + " ON " + ScopeText(g);
  }
  DbLogInfo(logcat::kAuth, "GRANT " + std::string(PrivName(g.priv)) + " ON " + ScopeText(g) + " TO " +
                               g.user);
  return DbStatus::Ok();
}

DbStatus AuthStore::RevokePrivilege(const std::string& user, const std::string& scope_db,
                                    const std::string& scope_table, Priv priv, std::string* note) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  AuthGrant g;
  const DbStatus rs = ResolveUserName(user, &g.user);
  if (!rs.ok()) {
    return rs;
  }
  g.scope_db = scope_db;
  g.scope_table = scope_table;
  g.priv = priv;

  const storage::Rid rid = FindGrantRow(g);
  if (!rid.IsValid()) {
    if (note != nullptr) {
      *note = "该授权本就不存在（无变化）";
    }
    return DbStatus::Ok();
  }
  const DbStatus ds = DeleteGrantRow(rid);
  if (!ds.ok()) {
    return ds;
  }
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    for (auto it = grants_.begin(); it != grants_.end(); ++it) {
      if (SameGrantKey(*it, g)) {
        grants_.erase(it);
        break;
      }
    }
  }
  if (note != nullptr) {
    *note = "已撤销 " + g.user + " 的 " + PrivName(g.priv) + " ON " + ScopeText(g);
  }
  DbLogInfo(logcat::kAuth, "REVOKE " + std::string(PrivName(g.priv)) + " ON " + ScopeText(g) +
                               " FROM " + g.user);
  return DbStatus::Ok();
}

DbStatus AuthStore::SetAdmin(const std::string& user, bool is_admin, std::string* note) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "身份库未附加存储引擎");
  }
  const std::string key = CanonicalName(user);
  AuthUser updated;
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    const auto it = users_.find(key);
    if (it == users_.end()) {
      return DbStatus::Error(DbCode::kUserError, "用户不存在: " + user);
    }
    if (it->second.is_admin == is_admin) {
      if (note != nullptr) {
        *note = "用户 " + it->second.name + (is_admin ? " 已是管理员" : " 本就不是管理员");
      }
      return DbStatus::Ok();
    }
    if (!is_admin && admin_count() <= 1) {
      return DbStatus::Error(DbCode::kLastAdmin, "不能撤销最后一个管理员: " + it->second.name);
    }
    updated = it->second;
  }
  updated.is_admin = is_admin;
  const DbStatus ws = UpdateUserRow(updated);
  if (!ws.ok()) {
    return ws;
  }
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    users_[key] = updated;
  }
  if (note != nullptr) {
    *note = "用户 " + updated.name + (is_admin ? " 已提升为管理员" : " 已撤销管理员");
  }
  DbLogInfo(logcat::kAuth, "SET ADMIN " + std::to_string(is_admin ? 1 : 0) + ": " + updated.name);
  return DbStatus::Ok();
}

// ── 只读查询 ────────────────────────────────────────────────

bool AuthStore::HasUser(const std::string& name) const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  return users_.count(CanonicalName(name)) != 0;
}

bool AuthStore::IsAdmin(const std::string& name) const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const auto it = users_.find(CanonicalName(name));
  return it != users_.end() && it->second.is_admin;
}

bool AuthStore::Authenticate(const std::string& name, const std::string& password,
                             bool* out_is_admin) const {
  std::string stored;
  {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    const auto it = users_.find(CanonicalName(name));
    if (it == users_.end()) {
      return false;  // 不区分「无此用户」与「口令错误」——避免用户名枚举
    }
    stored = it->second.pwd;
    if (out_is_admin != nullptr) {
      *out_is_admin = it->second.is_admin;
    }
  }
  return VerifyPassword(password, stored);
}

std::vector<std::string> AuthStore::ListUsers() const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  std::vector<std::string> out;
  out.reserve(users_.size());
  for (const auto& kv : users_) {
    out.push_back(kv.second.name);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return CanonicalName(a) < CanonicalName(b);
  });
  return out;
}

size_t AuthStore::user_count() const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  return users_.size();
}

std::vector<AuthGrant> AuthStore::GrantsOf(const std::string& user) const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const std::string key = CanonicalName(user);
  std::vector<AuthGrant> out;
  for (const AuthGrant& g : grants_) {
    if (CanonicalName(g.user) == key) {
      out.push_back(g);
    }
  }
  return out;
}

bool AuthStore::HasPrivilege(const std::string& user, const std::string& db, const std::string& table,
                             Priv need) const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const std::string key = CanonicalName(user);
  for (const AuthGrant& g : grants_) {
    if (CanonicalName(g.user) != key) {
      continue;
    }
    if (GrantSatisfies(g, db, table, need)) {
      return true;
    }
  }
  return false;
}

bool AuthStore::HasAnyPrivilegeOnDb(const std::string& user, const std::string& db) const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const std::string key = CanonicalName(user);
  for (const AuthGrant& g : grants_) {
    if (CanonicalName(g.user) != key) {
      continue;
    }
    if (g.scope_db == "*") {
      return true;  // 全局授权：任何库都算「有权限」
    }
    if (CanonicalName(g.scope_db) == CanonicalName(db)) {
      return true;
    }
  }
  return false;
}

std::vector<AuthUser> AuthStore::SnapshotUsers() const {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  std::vector<AuthUser> out;
  out.reserve(users_.size());
  for (const auto& kv : users_) {
    out.push_back(kv.second);
  }
  std::sort(out.begin(), out.end(), [](const AuthUser& a, const AuthUser& b) {
    return CanonicalName(a.name) < CanonicalName(b.name);
  });
  return out;
}

size_t AuthStore::admin_count() const {
  // 注意：调用方可能已持 mutex_（递归锁，安全）
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  size_t n = 0;
  for (const auto& kv : users_) {
    if (kv.second.is_admin) {
      ++n;
    }
  }
  return n;
}

}  // namespace cella::db
