// auth_parser.h —— 访问控制语句的识别与解析。
//
// 这些语句编译器不认识（它只认 CREATE TABLE / INSERT / GET / ...），因此在会话层
// 拦截后由本模块直接解析执行，与 BEGIN / CREATE DATABASE 的处理方式同构。
//
// 覆盖的语句（大小写不敏感）：
//   CREATE USER <名> [IDENTIFIED BY '<口令>']
//   DROP USER <名>
//   SET PASSWORD [FOR <名>] = '<口令>'      -- 省略 FOR = 改自己的口令
//   SHOW USERS
//   GRANT  <权限>[, <权限>]... ON <作用域> TO   <用户>[, <用户>]...
//   REVOKE <权限>[, <权限>]... ON <作用域> FROM <用户>[, <用户>]...
//   SHOW GRANTS [FOR <名>]
//
// 解析器自带一个极小的 tokenizer（词 / 单引号字符串 / 标点），不依赖编译器关键字表
// ——这样管理员/用户名可以是任意合法标识符（含保留字），也不需要动编译器。
#pragma once

#include <string>
#include <vector>

namespace cella::db {

// ── 用户管理语句 ────────────────────────────────────────────
struct UserCommand {
  enum class Kind {
    kCreateUser,   // name, password（password 可缺省 = 空口令）
    kDropUser,     // name
    kSetPassword,  // name（空 = 自己）, password
    kShowUsers,    // 无参数
  };
  Kind kind = Kind::kCreateUser;
  std::string name;      // 目标用户（原始拼写）
  std::string password;  // 明文口令（仅来自语句，不落盘）
  bool has_password = false;
};

// ── 授权语句 ────────────────────────────────────────────────
struct GrantCommand {
  enum class Kind {
    kGrant,       // 授予
    kRevoke,      // 撤销
    kShowGrants,  // 列出（name 为空 = 自己）
  };
  Kind kind = Kind::kGrant;
  std::vector<std::string> privs;   // "GET" / "INSERT" / ... / "ALL" / "ADMIN"（大写）
  std::string scope_db;             // "*" = 全局；否则库名
  std::string scope_table;          // "*" = 整库；否则表名
  std::vector<std::string> users;   // 目标用户（原始拼写）
  std::string name;                 // kShowGrants 的 FOR 目标（原始拼写，可空）
};

// 解析三态：不是本模块的语句 / 解析成功 / 是但写法有误
enum class AuthParse {
  kNotAuth,
  kOk,
  kSyntaxError,
};

AuthParse ParseUserCommand(const std::string& stmt_text, UserCommand* out, std::string* err);
AuthParse ParseGrantCommand(const std::string& stmt_text, GrantCommand* out, std::string* err);

// 作用域 → 文本（"*.*" / "lib.*" / "lib.t"），用于 SHOW GRANTS 与诊断
std::string FormatScope(const std::string& scope_db, const std::string& scope_table);

}  // namespace cella::db
