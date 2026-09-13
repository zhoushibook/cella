// auth_store.h —— 身份与权限库（`<data_dir>/cella_auth.db`）。
//
// 为什么是独立文件？
//   用户是「服务器级」概念：切库（USE）不换身份，授权可跨库。若把它塞进某个库的
//   系统表，切库就等于换了一整套用户，GRANT 也无法跨库表达。
//
// 持久化方式：复用页式存储（与 cella_catalog 同一套做法）——建两张系统表，
//   cella_users       (name VARCHAR(64), pwd VARCHAR(0), is_admin INT, created_at INT)
//   cella_privileges  (user VARCHAR(64), scope_db VARCHAR(64), scope_table VARCHAR(64),
//                      priv VARCHAR(16), grantable INT)
// 密码列存的是 `sha256$iter$salt$hash`（见 password.h），明文字节不落盘。
//
// 线程与锁约定（重要）：
//   * 内存缓存由本类内部的 mutex_（叶子锁）保护，只读查询（Authenticate / IsAdmin /
//     HasUser / ListUsers）在内部加锁，可从任意线程安全调用；
//   * 触达存储的写操作（EnsureTables / LoadFromStorage / CreateUser / ...）**要求调用方
//     已持有 DbEngine::storage_mutex_**（与 CatalogManager 的约定一致）。
//     锁序固定为 storage_mutex_ → mutex_，本类任何方法都不会反向获取 storage_mutex_。
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cella/db/common/db_status.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/types.h"

namespace cella::storage {
class IStorage;
}  // namespace cella::storage

namespace cella::db {

// ── 一个用户 ────────────────────────────────────────────────
struct AuthUser {
  std::string name;        // 原始拼写
  std::string pwd;         // sha256$iter$salt$hash
  bool is_admin = false;
  int64_t created_at = 0;  // Unix 秒
};

// ── 身份库 ──────────────────────────────────────────────────
class AuthStore {
 public:
  static constexpr const char* kFileName = "cella_auth.db";
  static constexpr const char* kUsersTable = "cella_users";
  static constexpr const char* kPrivilegesTable = "cella_privileges";
  static constexpr const char* kDefaultAdmin = "root";
  static constexpr size_t kMaxNameLen = 64;

  AuthStore() = default;
  AuthStore(const AuthStore&) = delete;
  AuthStore& operator=(const AuthStore&) = delete;

  void AttachStorage(storage::IStorage* storage) { storage_ = storage; }
  bool attached() const { return storage_ != nullptr; }

  // ── 生命周期（须持 storage_mutex_）─────────────────────────
  // 建两张系统表（缺哪张建哪张）；created 返回本次是否新建过
  DbStatus EnsureTables(bool* created);
  // 从存储重建内存缓存（先清空）
  DbStatus LoadFromStorage();

  // ── 用户管理（须持 storage_mutex_）────────────────────────
  DbStatus CreateUser(const std::string& name, const std::string& password, bool is_admin,
                      std::string* note);
  DbStatus DropUser(const std::string& name, std::string* note);
  DbStatus SetPassword(const std::string& name, const std::string& password, std::string* note);

  // ── 只读查询（内部加锁，线程安全）─────────────────────────
  bool HasUser(const std::string& name) const;
  bool IsAdmin(const std::string& name) const;
  // 认证：成功返回 true 并给出管理员标志；失败返回 false（不区分用户名/口令错）
  bool Authenticate(const std::string& name, const std::string& password,
                    bool* out_is_admin) const;
  std::vector<std::string> ListUsers() const;  // 按名（不区分大小写）排序
  // 用户快照（拷贝，按名排序）——供 SHOW USERS / 诊断使用，避免指针悬空
  std::vector<AuthUser> SnapshotUsers() const;
  size_t user_count() const;
  // 管理员数量（用于「不能删最后一个管理员」判定）
  size_t admin_count() const;

  // ── 工具 ──────────────────────────────────────────────────
  // 用户名合法性：[A-Za-z_][A-Za-z0-9_]*，长度 1..64
  static bool ValidUserName(const std::string& name);
  // 查找用的规范键（大写）
  static std::string CanonicalName(const std::string& name);

 private:
  static storage::Schema UsersSchema();
  static storage::Schema PrivilegesSchema();
  DbStatus InsertUserRow(const AuthUser& user);
  DbStatus UpdateUserRow(const AuthUser& user);
  DbStatus DeleteUserRow(const std::string& name);
  storage::Rid FindUserRow(const std::string& name) const;

  mutable std::recursive_mutex mutex_;  // 叶子锁：只保护内存缓存
  storage::IStorage* storage_ = nullptr;
  std::map<std::string, AuthUser> users_;  // 键 = 大写用户名
};

}  // namespace cella::db
