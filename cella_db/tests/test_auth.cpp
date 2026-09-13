// test_auth.cpp —— 访问控制测试（M1：认证）。
//
// 覆盖：口令哈希自检（含 SHA-256 已知向量）、管理员引导、登录 / 未登录拦截、
//       用户管理（建 / 删 / 改口令）、非管理员受限、最后一个管理员、持久化、
//       以及「开关关闭时行为与引入本特性前一致」。
#include <string>

#include "cella/db/auth/password.h"
#include "mini_test.h"
#include "test_util.h"

using cella::db::DbCode;
using cella::db::HashPassword;
using cella::db::PasswordIterations;
using cella::db::ScriptReport;
using cella::db::Sha256Hex;
using cella::db::VerifyPassword;
using testutil::Engine;

// ── 口令哈希 ────────────────────────────────────────────────
MT_TEST(认证_口令哈希自检) {
  // SHA-256 已知向量（先验证实现本身没写错）
  MT_EQ(Sha256Hex("abc"),
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  MT_EQ(Sha256Hex(""),
        std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  const std::string h1 = HashPassword("secret");
  MT_CHECK(VerifyPassword("secret", h1));
  MT_CHECK(!VerifyPassword("Secret", h1));
  MT_CHECK(!VerifyPassword("", h1));
  MT_EQ(PasswordIterations(h1), 10000);

  // 同口令两次哈希：盐不同 → 摘要不同（防彩虹表）
  const std::string h2 = HashPassword("secret");
  MT_CHECK(h1 != h2);
  MT_CHECK(VerifyPassword("secret", h2));

  // 格式非法一律 false
  MT_CHECK(!VerifyPassword("secret", "not-a-hash"));
  MT_CHECK(!VerifyPassword("secret", "sha256$0$00$00"));
  MT_CHECK(!VerifyPassword("secret", "sha256$10$zz$" + std::string(64, '0')));
}

// ── 引导与登录 ──────────────────────────────────────────────
MT_TEST(认证_引导root与登录) {
  Engine e("auth_bootstrap", 32, true);
  MT_CHECK(e.opened);
  // 首次开启认证 → 自动建 root（空口令），并留下安全提示
  MT_CHECK(e.engine.auth().HasUser("root"));
  MT_CHECK(e.engine.auth().IsAdmin("root"));
  MT_CHECK(!e.engine.auth_bootstrap_note().empty());

  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.session().is_admin());
  MT_CHECK(!e.Login("root", "wrong"));
  MT_CHECK(!e.Login("nobody", ""));  // 用户名不存在与口令错误一样「认证失败」
  MT_CHECK(!e.Login("nobody", "x"));
}

MT_TEST(认证_未登录被拒) {
  Engine e("auth_nologin", 32, true);
  MT_CHECK(e.opened);
  // 未设置身份 → 任何语句都被拒（含事务控制与查询）
  const ScriptReport r = e.Run("BEGIN;");
  MT_CHECK(r.statements.size() == 1u);
  MT_CHECK(r.statements[0].status.code() == DbCode::kNoCredentials);
  MT_CHECK(e.Run("get * in cella_catalog;").statements[0].status.code() ==
           DbCode::kNoCredentials);
  // 登录后放行
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("get * in cella_catalog;").all_ok());
}

MT_TEST(认证_开关关闭时免登录) {
  Engine e("auth_off");
  MT_CHECK(e.opened);
  MT_CHECK(!e.engine.auth_enabled());
  MT_CHECK(e.session().is_admin());  // 关闭时一律按管理员
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
  // 用户语句本身始终可用（按需创建身份库）
  MT_CHECK(e.Run("CREATE USER alice IDENTIFIED BY 'pw';").all_ok());
  MT_CHECK(e.engine.auth().HasUser("alice"));
  const ScriptReport su = e.Run("SHOW USERS;");
  MT_CHECK(su.all_ok());
  MT_CHECK(su.statements[0].result.IsQuery());
}

// ── 用户管理与口令 ──────────────────────────────────────────
MT_TEST(认证_用户管理与口令) {
  Engine e("auth_user", 32, true);
  MT_CHECK(e.Login("root", ""));

  MT_CHECK(e.Run("CREATE USER alice IDENTIFIED BY 'pw1';").all_ok());
  MT_CHECK(e.engine.auth().HasUser("alice"));
  MT_CHECK(!e.engine.auth().IsAdmin("alice"));
  // 重复创建 / 非法用户名 → DB-803
  MT_CHECK(e.Run("CREATE USER alice IDENTIFIED BY 'x';").statements[0].status.code() ==
           DbCode::kUserError);
  MT_CHECK(e.Run("CREATE USER 1bad IDENTIFIED BY 'x';").statements[0].status.code() ==
           DbCode::kUserError);

  // 新用户可登录
  MT_CHECK(e.Login("alice", "pw1"));
  MT_CHECK(!e.Login("alice", "bad"));

  // 管理员代改口令
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("SET PASSWORD FOR alice = 'pw2';").all_ok());
  MT_CHECK(e.Login("alice", "pw2"));
  MT_CHECK(!e.Login("alice", "pw1"));

  // 自己改自己（不带 FOR）
  MT_CHECK(e.Run("SET PASSWORD = 'pw3';").all_ok());
  MT_CHECK(e.Login("alice", "pw3"));

  // SHOW USERS
  MT_CHECK(e.Login("root", ""));
  const ScriptReport su = e.Run("SHOW USERS;");
  MT_CHECK(su.all_ok());
  MT_EQ(testutil::ColsText(su.statements[0].result), std::string("user|admin|created_at"));
  MT_EQ(testutil::RowCount(su.statements[0].result), 2u);

  // 删除用户
  MT_CHECK(e.Run("DROP USER alice;").all_ok());
  MT_CHECK(!e.engine.auth().HasUser("alice"));
  MT_CHECK(e.Run("DROP USER alice;").statements[0].status.code() == DbCode::kUserError);
}

MT_TEST(认证_语句语法错误报DB_501) {
  Engine e("auth_syntax", 32, true);
  MT_CHECK(e.Login("root", ""));
  // 是用户管理语句但写法有误 → 明确的语法错误（不会被丢给编译器报奇怪的话）
  MT_CHECK(e.Run("CREATE USER;").statements[0].status.code() == DbCode::kSqlError);
  MT_CHECK(e.Run("SET PASSWORD alice = 'x';").statements[0].status.code() == DbCode::kSqlError);
  MT_CHECK(e.Run("CREATE USER x IDENTIFIED 'p';").statements[0].status.code() == DbCode::kSqlError);
  MT_CHECK(e.Run("DROP USER;").statements[0].status.code() == DbCode::kSqlError);
  // 普通 SQL 不受影响
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
}

// ── 权限受限与管理员保护 ────────────────────────────────────
MT_TEST(认证_非管理员受限) {
  Engine e("auth_priv", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE USER bob IDENTIFIED BY 'b';").all_ok());

  MT_CHECK(e.Login("bob", "b"));
  MT_CHECK(!e.session().is_admin());
  // 普通用户不能管理用户 → DB-805
  MT_CHECK(e.Run("CREATE USER eve IDENTIFIED BY 'e';").statements[0].status.code() ==
           DbCode::kGrantDenied);
  MT_CHECK(e.Run("DROP USER root;").statements[0].status.code() == DbCode::kGrantDenied);
  MT_CHECK(e.Run("SET PASSWORD FOR root = 'x';").statements[0].status.code() ==
           DbCode::kGrantDenied);
  // 但可以改自己的口令
  MT_CHECK(e.Run("SET PASSWORD = 'b2';").all_ok());
  MT_CHECK(e.Login("bob", "b2"));
  MT_CHECK(!e.Login("bob", "b"));
}

MT_TEST(认证_最后一个管理员) {
  Engine e("auth_lastadmin", 32, true);
  MT_CHECK(e.Login("root", ""));
  // 只剩 root 一个管理员：删自己 → DB-804，且 root 仍在
  MT_CHECK(e.Run("DROP USER root;").statements[0].status.code() == DbCode::kLastAdmin);
  MT_CHECK(e.engine.auth().HasUser("root"));
}

// ── 持久化 ──────────────────────────────────────────────────
MT_TEST(认证_持久化) {
  Engine e("auth_persist", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE USER carol IDENTIFIED BY 'c1';").all_ok());
  MT_CHECK(e.Run("SET PASSWORD = 'root2';").all_ok());  // root 口令也应持久化
  e.Close();

  MT_CHECK(e.Reopen());
  MT_CHECK(e.Login("carol", "c1"));  // 登录顺带打开身份库
  MT_CHECK(e.engine.auth().HasUser("root"));
  MT_CHECK(e.Login("root", "root2"));
  MT_CHECK(!e.Login("root", ""));  // 旧（空）口令失效
  MT_CHECK(e.engine.auth_bootstrap_note().empty());  // 已有用户 → 不再引导
}
