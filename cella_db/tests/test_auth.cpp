// test_auth.cpp —— 访问控制测试（M1：认证）。
//
// 覆盖：口令哈希自检（含 SHA-256 已知向量）、管理员引导、登录 / 未登录拦截、
//       用户管理（建 / 删 / 改口令）、非管理员受限、最后一个管理员、持久化、
//       以及「开关关闭时行为与引入本特性前一致」。
#include <filesystem>
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

// ═════════════════════════ 授权（M2）═════════════════════════

MT_TEST(授权_表级读写) {
  Engine e("auth_g_table", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE TABLE t(id INT, v VARCHAR(8));INSERT INTO t VALUES (1,'a');").all_ok());
  MT_CHECK(e.Run("CREATE USER alice IDENTIFIED BY 'p';").all_ok());

  // 未授权：读被拒
  MT_CHECK(e.Login("alice", "p"));
  MT_CHECK(e.Run("get * in t;").statements[0].status.code() == DbCode::kPermissionDenied);
  // 授权读 → 读通过，写仍被拒
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("GRANT get ON main.t TO alice;").all_ok());
  MT_CHECK(e.Login("alice", "p"));
  MT_CHECK(e.Run("get * in t;").all_ok());
  MT_CHECK(e.Run("INSERT INTO t VALUES (2,'b');").statements[0].status.code() ==
           DbCode::kPermissionDenied);
  MT_CHECK(e.Run("UPDATE t SET v = 'x';").statements[0].status.code() == DbCode::kPermissionDenied);
  MT_CHECK(e.Run("DELETE in t;").statements[0].status.code() == DbCode::kPermissionDenied);
  // 授权写（三种各自独立）
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("GRANT insert, update, delete ON main.t TO alice;").all_ok());
  MT_CHECK(e.Login("alice", "p"));
  MT_CHECK(e.Run("INSERT INTO t VALUES (2,'b');").all_ok());
  MT_CHECK(e.Run("UPDATE t SET v = 'c' limit id = 2;").all_ok());
  MT_CHECK(e.Run("DELETE in t limit id = 2;").all_ok());
  MT_EQ(testutil::RowsText(e.Run("get * in t;").statements[0].result), std::string("1|a"));
}

MT_TEST(授权_作用域与撤销) {
  Engine e("auth_g_scope", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE TABLE t(id INT);CREATE TABLE u(id INT);").all_ok());
  MT_CHECK(e.Run("CREATE USER bob IDENTIFIED BY 'p';").all_ok());
  // 整库只读：库里所有表都可读
  MT_CHECK(e.Run("GRANT get ON main.* TO bob;").all_ok());
  MT_CHECK(e.Login("bob", "p"));
  MT_CHECK(e.Run("get * in t;").all_ok());
  MT_CHECK(e.Run("get * in u;").all_ok());
  // 全局只读（`*.*`）：仍然只读，写被拒
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("GRANT get ON *.* TO bob;").all_ok());
  MT_CHECK(e.Login("bob", "p"));
  MT_CHECK(e.Run("get * in t;").all_ok());
  MT_CHECK(e.Run("INSERT INTO t VALUES (9);").statements[0].status.code() ==
           DbCode::kPermissionDenied);
  // 撤销即时生效（整库 + 全局都撤掉）
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("REVOKE get ON main.* FROM bob;").all_ok());
  MT_CHECK(e.Run("REVOKE get ON *.* FROM bob;").all_ok());
  MT_CHECK(e.Login("bob", "p"));
  MT_CHECK(e.Run("get * in t;").statements[0].status.code() == DbCode::kPermissionDenied);
  // 撤销不存在的授权：幂等成功
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("REVOKE get ON main.t FROM bob;").all_ok());
}

MT_TEST(授权_建表删表需库级权限) {
  Engine e("auth_g_ddl", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
  MT_CHECK(e.Run("CREATE USER carol IDENTIFIED BY 'p';").all_ok());
  // 只有表级 DML 权限 → 建表/删表被拒（库级需求）
  MT_CHECK(e.Run("GRANT all ON main.t TO carol;").all_ok());
  MT_CHECK(e.Login("carol", "p"));
  MT_CHECK(e.Run("CREATE TABLE t2(id INT);").statements[0].status.code() ==
           DbCode::kPermissionDenied);
  MT_CHECK(e.Run("DROP TABLE t;").statements[0].status.code() == DbCode::kPermissionDenied);
  // 库级 all → 建表 / 删表放行
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("GRANT all ON main.* TO carol;").all_ok());
  MT_CHECK(e.Login("carol", "p"));
  MT_CHECK(e.Run("CREATE TABLE t2(id INT);").all_ok());
  MT_CHECK(e.Run("DROP TABLE t2;").all_ok());
}

MT_TEST(授权_跨库) {
  Engine e("auth_g_cross", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE TABLE t(id INT);INSERT INTO t VALUES (1);CREATE TABLE u(id INT);"
                 "CREATE DATABASE b;USE b;CREATE TABLE bt(id INT);INSERT INTO bt VALUES (7);"
                 "USE main;")
               .all_ok());
  MT_CHECK(e.Run("CREATE USER dave IDENTIFIED BY 'p';").all_ok());
  MT_CHECK(e.Run("GRANT get ON b.* TO dave;").all_ok());
  MT_CHECK(e.Run("GRANT get ON main.t TO dave;").all_ok());

  MT_CHECK(e.Login("dave", "p"));
  // b 库：整库可读
  MT_CHECK(e.Run("USE b;").all_ok());
  MT_CHECK(e.Run("get * in bt;").all_ok());
  // main 库：只有 t 可读（有 main.t 授权 → 也就能切过去）
  MT_CHECK(e.Run("USE main;").all_ok());
  MT_CHECK(e.Run("get * in t;").all_ok());
  MT_CHECK(e.Run("get * in u;").statements[0].status.code() == DbCode::kPermissionDenied);
  // SHOW DATABASES：只列出有权限的库（b 与 main），不含身份库
  MT_EQ(testutil::RowsText(e.Run("SHOW DATABASES;").statements[0].result), std::string("b\nmain"));
  // 建库需要管理员；切到无授权的库被拒
  MT_CHECK(e.Run("CREATE DATABASE c;").statements[0].status.code() == DbCode::kPermissionDenied);
  MT_CHECK(e.Run("USE c;").statements[0].status.code() == DbCode::kPermissionDenied);
}

MT_TEST(授权_库管理需管理员) {
  Engine e("auth_g_db", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE USER eve IDENTIFIED BY 'p';CREATE DATABASE d1;").all_ok());
  MT_CHECK(e.Run("GRANT get ON d1.* TO eve;").all_ok());
  MT_CHECK(e.Login("eve", "p"));
  MT_CHECK(e.Run("CREATE DATABASE d2;").statements[0].status.code() == DbCode::kPermissionDenied);
  MT_CHECK(e.Run("DROP DATABASE d1;").statements[0].status.code() == DbCode::kPermissionDenied);
  // 保留名：任何人都不能建
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE DATABASE cella_auth;").statements[0].status.code() ==
           DbCode::kDatabaseError);
  // SHOW DATABASES 里没有身份库
  const ScriptReport sd = e.Run("SHOW DATABASES;");
  MT_CHECK(sd.all_ok());
  MT_CHECK(testutil::RowsText(sd.statements[0].result).find("cella_auth") == std::string::npos);
}

MT_TEST(授权_授予需管理员) {
  Engine e("auth_g_who", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE USER frank IDENTIFIED BY 'p';CREATE TABLE t(id INT);").all_ok());
  MT_CHECK(e.Login("frank", "p"));
  MT_CHECK(e.Run("GRANT get ON main.t TO frank;").statements[0].status.code() ==
           DbCode::kGrantDenied);
  MT_CHECK(e.Run("REVOKE get ON main.t FROM frank;").statements[0].status.code() ==
           DbCode::kGrantDenied);
  // 查看自己的授权是允许的（此时为空）
  const ScriptReport sg = e.Run("SHOW GRANTS;");
  MT_CHECK(sg.all_ok());
  MT_EQ(testutil::ColsText(sg.statements[0].result), std::string("scope|priv"));
  MT_EQ(testutil::RowCount(sg.statements[0].result), 0u);
  // 不能查看他人授权
  MT_CHECK(e.Run("SHOW GRANTS FOR root;").statements[0].status.code() == DbCode::kGrantDenied);
}

MT_TEST(授权_管理员权限与最后一个管理员) {
  Engine e("auth_g_admin", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE USER gina IDENTIFIED BY 'p';").all_ok());
  MT_CHECK(e.Run("GRANT admin TO gina;").all_ok());
  MT_CHECK(e.engine.auth().IsAdmin("gina"));
  // 提升后 gina 可以管用户（无需任何表级授权）
  MT_CHECK(e.Login("gina", "p"));
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
  MT_CHECK(e.Run("CREATE USER hank IDENTIFIED BY 'p';").all_ok());
  // 不能撤销自己的管理员
  MT_CHECK(e.Run("REVOKE admin FROM gina;").statements[0].status.code() == DbCode::kLastAdmin);
  MT_CHECK(e.engine.auth().IsAdmin("gina"));
  // root 降级会被拒（撤销后仍有 gina 是管理员，故此时允许——验证「非最后一个」放行）
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("REVOKE admin FROM gina;").all_ok());
  MT_CHECK(!e.engine.auth().IsAdmin("gina"));
  // 只剩 root 一个管理员：撤销自己 → DB-804
  MT_CHECK(e.Run("REVOKE admin FROM root;").statements[0].status.code() == DbCode::kLastAdmin);
}

MT_TEST(授权_SHOW_GRANTS与语法) {
  Engine e("auth_g_show", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE TABLE t(id INT);CREATE USER ivy IDENTIFIED BY 'p';").all_ok());
  MT_CHECK(e.Run("GRANT get, insert ON main.t TO ivy;").all_ok());
  MT_CHECK(e.Run("GRANT get ON main.* TO ivy;").all_ok());
  const ScriptReport sg = e.Run("SHOW GRANTS FOR ivy;");
  MT_CHECK(sg.all_ok());
  MT_EQ(testutil::ColsText(sg.statements[0].result), std::string("scope|priv"));
  MT_CHECK(testutil::RowCount(sg.statements[0].result) == 3u);  // main.t get/main.t insert/main.* get
  // 管理员查看自己：含 ADMIN 行
  const ScriptReport sr = e.Run("SHOW GRANTS;");
  MT_CHECK(sr.all_ok());
  MT_EQ(testutil::RowsText(sr.statements[0].result), std::string("*.*|ADMIN"));
  // 语法错误 → DB-501（明确的语法错误而非丢给编译器）
  MT_CHECK(e.Run("GRANT get TO ivy;").statements[0].status.code() == DbCode::kSqlError);
  MT_CHECK(e.Run("GRANT bogus ON main.t TO ivy;").statements[0].status.code() == DbCode::kSqlError);
  MT_CHECK(e.Run("GRANT get ON t TO;").statements[0].status.code() == DbCode::kSqlError);
  // 用户不存在 → DB-803
  MT_CHECK(e.Run("GRANT get ON main.t TO nobody;").statements[0].status.code() ==
           DbCode::kUserError);
}

MT_TEST(授权_持久化与系统表放行) {
  Engine e("auth_g_persist", 32, true);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
  MT_CHECK(e.Run("CREATE USER jack IDENTIFIED BY 'p';").all_ok());
  MT_CHECK(e.Run("GRANT get ON main.t TO jack;").all_ok());
  e.Close();
  MT_CHECK(e.Reopen());

  MT_CHECK(e.Login("jack", "p"));
  MT_CHECK(e.Run("get * in t;").all_ok());                      // 授权跨重启仍生效
  MT_CHECK(e.Run("get * in cella_catalog;").all_ok());          // 系统表读放行
  MT_CHECK(e.Run("CREATE TABLE x(id INT);").statements[0].status.code() ==
           DbCode::kPermissionDenied);
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("REVOKE get ON main.t FROM jack;").all_ok());  // 撤销也持久化
  MT_CHECK(e.Login("jack", "p"));
  MT_CHECK(e.Run("get * in t;").statements[0].status.code() == DbCode::kPermissionDenied);
}

// ── 建库健壮性：首次创建就必须完整落盘（回归「半成品身份库」）──────
MT_TEST(认证_首次创建即完整落盘) {
  Engine e("auth_flush", 32, true);
  const std::string path = e.cfg.data_dir + "/cella_auth.db";
  MT_CHECK(std::filesystem::exists(path));
  // 新建文件时元数据页先写盘、表页只随 Close 刷出 —— 若不在创建后立即刷全，
  // 进程被打断就会留下「只有元数据页」的半成品（此后每次打开都报读页失败）。
  // 因此这里断言：**引擎还开着**的时候，文件就已经是完整的多页状态。
  const auto size = static_cast<long long>(std::filesystem::file_size(path));
  MT_CHECK(size > 4096);  // 至少两页：元数据页 + 表页
  // 且 flush 之后仍能正常工作（root 已随第一次落盘写入）
  MT_CHECK(e.engine.auth().HasUser("root"));
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("CREATE USER alice IDENTIFIED BY 'p';").all_ok());

  // 关掉重开：文件自洽，数据仍在
  e.Close();
  MT_CHECK(e.Reopen());
  MT_CHECK(e.Login("alice", "p"));
  MT_CHECK(e.Login("root", ""));
  MT_CHECK(e.Run("SHOW USERS;").all_ok());
}
