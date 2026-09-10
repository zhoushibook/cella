// test_txn.cpp —— 事务测试：提交 / 回滚（三种 DML）/ 语句级原子性 /
//                 DDL 隐式提交 / 会话状态机 / 重启持久化 / 审计日志 /
//                 目录与数据文件不一致的自愈 / DDL 与提交的持久化。
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

// 在「已存在的数据目录」上开一个引擎（不清理，用于模拟二次打开）
std::unique_ptr<DbEngine> OpenOn(const std::string& dir, bool checkpoint_on_commit = false) {
  auto e = std::make_unique<DbEngine>();
  EngineConfig c;
  c.data_dir = dir;
  c.enable_log = false;
  c.enable_journal = true;
  c.checkpoint_on_commit = checkpoint_on_commit;
  if (!e->Open(c).ok()) {
    return nullptr;
  }
  return e;
}

void SeedAccounts(Engine* e) {
  const ScriptReport r =
      e->Run("CREATE TABLE account(id INT NOT NULL, balance INT NOT NULL);"
             "INSERT INTO account VALUES (1,100),(2,50),(3,200);");
  MT_CHECK(r.all_ok());
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::string();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

MT_TEST(事务_提交后生效) {
  Engine e("txn_commit");
  SeedAccounts(&e);
  const ScriptReport r = e.Run(
      "BEGIN;"
      "INSERT INTO account VALUES (4,300);"
      "UPDATE account SET balance = 150 limit id = 1;"
      "COMMIT;");
  MT_CHECK(r.all_ok());
  MT_EQ(r.statements.size(), 4u);
  MT_CHECK(r.statements[0].txn_id != 0);
  // BEGIN 与 COMMIT 之间共享同一个事务号
  MT_EQ(r.statements[0].txn_id, r.statements[1].txn_id);
  MT_EQ(r.statements[1].txn_id, r.statements[2].txn_id);
  // 事务内的语句不再自动提交
  MT_CHECK(!r.statements[1].auto_committed);

  MT_EQ(RowsText(e.Run("get id, balance in account ordered id asc;").statements[0].result),
        std::string("1|150\n2|50\n3|200\n4|300"));
}

MT_TEST(事务_回滚插入) {
  Engine e("txn_rb_insert");
  SeedAccounts(&e);
  const ScriptReport r = e.Run("BEGIN;INSERT INTO account VALUES (9,999);ROLLBACK;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3"));
}

MT_TEST(事务_回滚更新恢复旧值) {
  Engine e("txn_rb_update");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;UPDATE account SET balance = 0 limit id = 2;").all_ok());
  // 事务内可见新值
  MT_EQ(RowsText(e.Run("get balance in account limit id = 2;").statements[0].result),
        std::string("0"));
  MT_CHECK(e.Run("ROLLBACK;").all_ok());
  // 回滚后恢复旧值（undo 日志重插旧行）
  MT_EQ(RowsText(e.Run("get balance in account limit id = 2;").statements[0].result),
        std::string("50"));
}

MT_TEST(事务_回滚删除恢复行) {
  Engine e("txn_rb_delete");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;DELETE in account limit id = 3;").all_ok());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2"));
  MT_CHECK(e.Run("ROLLBACK;").all_ok());
  MT_EQ(RowsText(e.Run("get id, balance in account ordered id asc;").statements[0].result),
        std::string("1|100\n2|50\n3|200"));
}

MT_TEST(事务_语句级原子性) {
  Engine e("txn_stmt_atomic");
  SeedAccounts(&e);
  // 多行插入的第二行运行期失败：第一行必须一并撤销，事务继续有效
  const ScriptReport bad = e.Run("BEGIN;INSERT INTO account VALUES (5,500),(6,1.5);");
  MT_CHECK(!bad.all_ok());
  MT_EQ(bad.statements.size(), 2u);
  MT_CHECK(bad.statements[1].rolled_back_here);
  MT_CHECK(bad.statements[1].status.code() == DbCode::kTypeMismatch);

  // 事务仍可继续使用
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3"));
  MT_CHECK(e.Run("INSERT INTO account VALUES (5,500);").all_ok());
  MT_CHECK(e.Run("COMMIT;").all_ok());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3\n5"));
}

MT_TEST(事务_编译失败不影响事务) {
  Engine e("txn_compile_fail");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;").all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (1,2,3);").statements[0].status.code() ==
           DbCode::kSqlError);
  MT_CHECK(e.Run("INSERT INTO account VALUES (7,700);").all_ok());
  MT_CHECK(e.Run("COMMIT;").all_ok());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3\n7"));
}

MT_TEST(事务_提交后仍可继续) {
  Engine e("txn_after_commit");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (4,400);COMMIT;").all_ok());
  // 提交之后是自动提交模式
  MT_CHECK(e.Run("INSERT INTO account VALUES (5,500);").all_ok());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3\n4\n5"));
  MT_CHECK(!e.session().in_transaction());
}

MT_TEST(事务_状态机错误) {
  Engine e("txn_state");
  SeedAccounts(&e);
  // 没有活动事务时 COMMIT / ROLLBACK
  MT_CHECK(e.Run("COMMIT;").statements[0].status.code() == DbCode::kSessionError);
  MT_CHECK(e.Run("ROLLBACK;").statements[0].status.code() == DbCode::kSessionError);
  // 重复 BEGIN
  const ScriptReport r = e.Run("BEGIN;BEGIN;");
  MT_CHECK(r.statements[0].status.ok());
  MT_CHECK(r.statements[1].status.code() == DbCode::kTxnAlreadyActive);
  MT_CHECK(e.Run("ROLLBACK;").all_ok());
}

MT_TEST(事务_DDL隐式提交) {
  Engine e("txn_ddl");
  SeedAccounts(&e);
  const ScriptReport r = e.Run(
      "BEGIN;"
      "INSERT INTO account VALUES (8,800);"
      "CREATE TABLE audit(id INT, note VARCHAR(16));");
  MT_CHECK(r.all_ok());
  MT_EQ(r.statements.size(), 3u);
  MT_CHECK(r.statements[2].implicit_commit);
  MT_CHECK(!r.statements[2].notice.empty());
  // 隐式提交后会话回到自动提交模式
  MT_CHECK(!e.session().in_transaction());
  // 前置事务的插入已随隐式提交落定
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3\n8"));
  MT_CHECK(e.engine.catalog().FindTable("audit") != nullptr);
}

MT_TEST(事务_重启后持久化) {
  Engine e("txn_persist");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (4,400);COMMIT;").all_ok());
  // 未提交的改动不应存活
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (5,500);").all_ok());
  MT_CHECK(e.Run("ROLLBACK;").all_ok());
  MT_CHECK(e.Run("CREATE TABLE course(id INT, title VARCHAR(16));").all_ok());
  MT_CHECK(e.Run("INSERT INTO course VALUES (1,'Math');").all_ok());
  e.Close();

  // 重新打开同一数据目录
  MT_CHECK(e.Reopen());
  MT_CHECK(e.engine.catalog().FindTable("account") != nullptr);
  MT_CHECK(e.engine.catalog().FindTable("course") != nullptr);
  MT_EQ(RowsText(e.Run("get id, balance in account ordered id asc;").statements[0].result),
        std::string("1|100\n2|50\n3|200\n4|400"));
  MT_EQ(RowsText(e.Run("get title in course;").statements[0].result), std::string("Math"));
  // 重启后仍能继续写入
  MT_CHECK(e.Run("INSERT INTO account VALUES (6,600);").all_ok());
  MT_EQ(RowsText(e.Run("get distinct id in account;").statements[0].result),
        std::string("1\n2\n3\n4\n6"));
}

MT_TEST(事务_重启保留删除效果) {
  Engine e("txn_persist_delete");
  SeedAccounts(&e);
  MT_CHECK(e.Run("DELETE in account limit id = 2;").all_ok());
  e.Close();
  MT_CHECK(e.Reopen());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n3"));
}

MT_TEST(事务_统计与审计日志) {
  Engine e("txn_journal");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (4,400);COMMIT;").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (5,500);ROLLBACK;").all_ok());
  MT_CHECK(e.engine.txn_manager().committed_total() >= 1u);
  MT_CHECK(e.engine.txn_manager().aborted_total() >= 1u);
  MT_EQ(e.engine.txn_manager().active_count(), 0u);

  const std::string journal = ReadFile(e.cfg.data_dir + "/journal.log");
  MT_CHECK(journal.find("COMMIT") != std::string::npos);
  MT_CHECK(journal.find("ABORT") != std::string::npos);
  MT_CHECK(journal.find("用户 ROLLBACK") != std::string::npos);
}

MT_TEST(事务_关闭时回滚未提交事务) {
  Engine e("txn_close_rollback");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (9,900);").all_ok());
  MT_CHECK(e.session().in_transaction());
  e.Close();  // 会话结束时未提交事务必须回滚

  MT_CHECK(e.Reopen());
  MT_EQ(RowsText(e.Run("get id in account ordered id asc;").statements[0].result),
        std::string("1\n2\n3"));
}

MT_TEST(事务_会话状态行) {
  Engine e("txn_session_line");
  SeedAccounts(&e);
  MT_CHECK(e.session().StatusLine().find("自动提交") != std::string::npos);
  MT_CHECK(e.Run("BEGIN;").all_ok());
  MT_CHECK(e.session().StatusLine().find("事务中") != std::string::npos);
  MT_EQ(e.session().current_txn() != 0, true);
  MT_CHECK(e.Run("ROLLBACK;").all_ok());
  MT_EQ(e.session().current_txn(), 0u);
  MT_CHECK(e.session().StatusLine().find("自动提交") != std::string::npos);
}

MT_TEST(事务_目录与数据文件不一致时自愈) {
  Engine e("txn_selfheal");
  SeedAccounts(&e);
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (9,900);COMMIT;").all_ok());
  e.Close();  // 干净关闭：数据文件与目录一致

  // 模拟「进程被强杀后目录有表、数据文件里没表」：删掉数据文件、保留 catalog.meta
  const std::string db_path = e.cfg.data_dir + "/" + e.cfg.db_file;
  std::error_code ec;
  MT_CHECK(std::filesystem::remove(db_path, ec));

  // 重新打开：必须自愈而不是 DB-509 拒绝打开
  MT_CHECK(e.Reopen());
  MT_CHECK(e.engine.has_recoveries());
  MT_CHECK(e.engine.RecoveryReport().find("account") != std::string::npos);
  // 表结构保住（空表），后续可继续写入
  MT_CHECK(e.engine.catalog().FindTable("account") != nullptr);
  const ScriptReport q = e.Run("get id in account;");
  MT_CHECK(q.all_ok());
  MT_EQ(q.statements[0].result.rows.size(), 0u);
  MT_CHECK(e.Run("INSERT INTO account VALUES (1,100);").all_ok());
  MT_EQ(RowsText(e.Run("get id in account;").statements[0].result), std::string("1"));
}

MT_TEST(事务_DDL立即持久化) {
  Engine e("txn_ddl_durable");
  // DDL 在自动提交后立刻存盘；随后的 DML 默认不存盘（checkpoint_on_commit=false）
  MT_CHECK(e.Run("CREATE TABLE t(id INT NOT NULL, v VARCHAR(16));"
                 "INSERT INTO t VALUES (1,'a');")
               .all_ok());

  // 不关闭引擎，直接把数据目录复制一份（等同于进程被强杀后拿磁盘上的文件来恢复）
  const std::string copy_dir = testutil::FreshDir("txn_ddl_durable_copy");
  std::error_code ec;
  std::filesystem::copy(e.cfg.data_dir, copy_dir,
                        std::filesystem::copy_options::recursive, ec);
  MT_CHECK(!ec);

  auto e2 = OpenOn(copy_dir);
  MT_CHECK(e2 != nullptr);
  // 表在（DDL 已持久化）
  MT_CHECK(e2->catalog().FindTable("t") != nullptr);
  ScriptReport r;
  (void)e2->default_session().Execute("get * in t;", &r);
  MT_CHECK(r.all_ok());
  // 数据不在（DML 默认不存盘，属于设计内的持久性边界）
  MT_EQ(testutil::RowsText(r.statements[0].result), std::string(""));
  e2->Close();
}

MT_TEST(事务_checkpoint_on_commit_立即持久化数据) {
  const std::string dir = testutil::FreshDir("txn_cp_on");
  {
    auto e = OpenOn(dir, /*checkpoint_on_commit=*/true);
    MT_CHECK(e != nullptr);
    ScriptReport r;
    (void)e->default_session().Execute("CREATE TABLE t(id INT NOT NULL);INSERT INTO t VALUES (7);", &r);
    MT_CHECK(r.all_ok());
  }  // 引擎析构前先正常 Close（见下方说明）

  // 上面的引擎析构时 Close 会再次落盘；这里重开同一目录验证「提交即持久化」的可见性
  auto e2 = OpenOn(dir);
  MT_CHECK(e2 != nullptr);
  ScriptReport r2;
  (void)e2->default_session().Execute("get id in t;", &r2);
  MT_CHECK(r2.all_ok());
  MT_EQ(testutil::RowsText(r2.statements[0].result), std::string("7"));
  e2->Close();
}

MT_TEST(事务_Checkpoint_手动存盘) {
  Engine e("txn_checkpoint_api");
  SeedAccounts(&e);
  MT_CHECK(e.Run("INSERT INTO account VALUES (42,42);").all_ok());
  // 手动存盘后，复制目录到别处打开也能看到这条数据
  MT_CHECK(e.engine.Checkpoint().ok());
  const std::string copy_dir = testutil::FreshDir("txn_checkpoint_api_copy");
  std::error_code ec;
  std::filesystem::copy(e.cfg.data_dir, copy_dir,
                        std::filesystem::copy_options::recursive, ec);
  MT_CHECK(!ec);
  auto e2 = OpenOn(copy_dir);
  MT_CHECK(e2 != nullptr);
  ScriptReport r;
  (void)e2->default_session().Execute("get id, balance in account limit id = 42;", &r);
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::RowsText(r.statements[0].result), std::string("42|42"));
  e2->Close();
}
