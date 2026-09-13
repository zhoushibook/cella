#include "cella/db/auth/privilege.h"

#include <cctype>

namespace cella::db {
namespace {

std::string Upper(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

bool IEquals(const std::string& a, const std::string& b) { return Upper(a) == Upper(b); }

}  // namespace

const char* PrivName(Priv p) {
  switch (p) {
    case Priv::kGet:    return "GET";
    case Priv::kInsert: return "INSERT";
    case Priv::kUpdate: return "UPDATE";
    case Priv::kDelete: return "DELETE";
    case Priv::kCreate: return "CREATE";
    case Priv::kDrop:   return "DROP";
    case Priv::kAll:    return "ALL";
    case Priv::kAdmin:  return "ADMIN";
  }
  return "?";
}

bool ParsePriv(const std::string& text, Priv* out) {
  const std::string u = Upper(text);
  Priv p = Priv::kGet;  // 初值仅为消除「可能未初始化」告警；下面每个分支都会赋值
  if (u == "GET" || u == "SELECT") {
    p = Priv::kGet;
  } else if (u == "INSERT") {
    p = Priv::kInsert;
  } else if (u == "UPDATE") {
    p = Priv::kUpdate;
  } else if (u == "DELETE") {
    p = Priv::kDelete;
  } else if (u == "CREATE") {
    p = Priv::kCreate;
  } else if (u == "DROP") {
    p = Priv::kDrop;
  } else if (u == "ALL") {
    p = Priv::kAll;
  } else if (u == "ADMIN") {
    p = Priv::kAdmin;
  } else {
    return false;
  }
  if (out != nullptr) {
    *out = p;
  }
  return true;
}

bool TablePrivToPriv(TablePriv need, Priv* out) {
  Priv p = Priv::kGet;
  switch (need) {
    case TablePriv::kRead:   p = Priv::kGet;    break;
    case TablePriv::kInsert: p = Priv::kInsert; break;
    case TablePriv::kUpdate: p = Priv::kUpdate; break;
    case TablePriv::kDelete: p = Priv::kDelete; break;
    case TablePriv::kCreate: p = Priv::kCreate; break;
    case TablePriv::kDrop:   p = Priv::kDrop;   break;
    case TablePriv::kNone:   return false;
  }
  if (out != nullptr) {
    *out = p;
  }
  return true;
}

bool ScopeCovers(const AuthGrant& g, const std::string& db, const std::string& table) {
  const bool db_level_need = (table == "*");
  if (g.scope_db == "*") {
    // 全局授权：只有 `*.*` 才有意义（解析器不接受 `*.表名`）
    return g.scope_table == "*";
  }
  if (!IEquals(g.scope_db, db)) {
    return false;
  }
  if (g.scope_table == "*") {
    return true;  // 整库：既覆盖库级需求，也覆盖库里所有表
  }
  if (db_level_need) {
    return false;  // 表级授权不能满足「建表/删表」这类库级需求
  }
  return IEquals(g.scope_table, table);
}

bool GrantSatisfies(const AuthGrant& g, const std::string& db, const std::string& table, Priv need) {
  if (need == Priv::kAdmin) {
    return false;  // 管理员由 is_admin 标志承载，权限行不表达它
  }
  if (g.priv != Priv::kAll && g.priv != need) {
    return false;
  }
  return ScopeCovers(g, db, table);
}

}  // namespace cella::db
