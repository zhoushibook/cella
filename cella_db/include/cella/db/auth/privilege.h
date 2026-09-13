// privilege.h —— 权限模型：权限名、作用域、覆盖判定（纯逻辑，不触存储）。
//
// 设计要点：
//   * 一条授权 = (用户, 作用域, 权限)。作用域三档：
//       `*.*`   全局——所有库的所有表，也满足「库级」需求（建表/删表）
//       `lib.*` 整库——该库的所有表 + 库级需求
//       `lib.t` 单表——只有该表的数据权限（不能满足库级需求）
//   * 判定取**最具体**的一条（表级 > 库级 > 全局）——由「任一条满足即通过」自然实现，
//     因为越具体的授权越容易命中。
//   * `ALL` 表示「该作用域上的全部权限」，但**不含** `ADMIN`
//     （管理员是用户属性，由 `GRANT ADMIN` 改的是 is_admin 标志，不落权限行）。
//   * 管理员与系统表读的短路判定不在这里，而在调用方（会话层 / 执行器）。
#pragma once

#include <string>
#include <vector>

namespace cella::db {

// ── 权限名 ──────────────────────────────────────────────────
enum class Priv {
  kGet,     // 读（`get` 语句；`SELECT` 是等价写法）
  kInsert,
  kUpdate,
  kDelete,
  kCreate,  // 建表（库级）
  kDrop,    // 删表（库级）
  kAll,     // 该作用域上的全部权限（不含 ADMIN）
  kAdmin,   // 管理员（不落权限行，改 is_admin 标志）
};

const char* PrivName(Priv p);
bool ParsePriv(const std::string& text, Priv* out);

// ── 一条授权（持久化在 cella_privileges 表）─────────────────
struct AuthGrant {
  std::string user;         // 被授权用户（原始拼写）
  std::string scope_db;     // "*" = 全局；否则库名
  std::string scope_table;  // "*" = 整库；否则表名
  Priv priv = Priv::kGet;
  bool grantable = false;   // WITH GRANT OPTION（本迭代只记录，不参与判定）
};

// ── 表锁对应的数据权限（执行器在 LockTable 这一必经点判定）──
enum class TablePriv { kNone, kRead, kInsert, kUpdate, kDelete, kCreate, kDrop };

// TablePriv → Priv；kNone 无对应权限（返回 false，表示「不检查」）
bool TablePrivToPriv(TablePriv need, Priv* out);

// 作用域是否覆盖 (db, table)；table = "*" 表示这是「库级需求」（建表/删表）
bool ScopeCovers(const AuthGrant& g, const std::string& db, const std::string& table);
// 一条授权是否满足 (db, table, need)
bool GrantSatisfies(const AuthGrant& g, const std::string& db, const std::string& table, Priv need);

}  // namespace cella::db
