// test_database.cpp —— SQL 级多库测试：CREATE/DROP DATABASE、USE、SHOW DATABASES、
//                      库间隔离、锁不串库、事务中禁切、软删除可恢复、旧布局迁移。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

// SHOW DATABASES 结果里的库名列表（字母序）
std::vector<std::string> DbNames(const ScriptReport& r) {
  std::vector<std::string> out;
  for (const auto& v : r.statements[0].result.rows) {
    out.push_back(v[0].str_val);
  }
  return out;
}

bool Contains(const std::vector<std::string>& list, const std::string& name) {
  for (const auto& s : list) {
    if (s == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

MT_TEST(多库_创建与列举) {
  Engine e("db_create_show");
  MT_CHECK(e.opened);
  MT_EQ(e.engine.current_db(), std::string("main"));

  MT_CHECK(e.Run("CREATE DATABASE school;").all_ok());
  MT_CHECK(e.Run("CREATE DATABASE shop;").all_ok());

  const ScriptReport show = e.Run("SHOW DATABASES;");
  MT_CHECK(show.all_ok());
  MT_EQ(testutil::ColsText(show.statements[0].result), std::string("name"));
  const std::vector<std::string> names = DbNames(show);
  MT_CHECK(Contains(names, "main"));
  MT_CHECK(Contains(names, "school"));
  MT_CHECK(Contains(names, "shop"));

  // 重复创建 → DB-514；库文件真实存在
  MT_CHECK(e.Run("CREATE DATABASE school;").statements[0].status.code() == DbCode::kDatabaseError);
  MT_CHECK(std::filesystem::exists(e.cfg.data_dir + "/school.db"));
}

MT_TEST(多库_切换与隔离) {
  Engine e("db_switch");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE t(id INT);INSERT INTO t VALUES (1);").all_ok());
  MT_CHECK(e.Run("CREATE DATABASE b;").all_ok());

  // 切到 b：看不到 main 的 t（缺表在语义阶段报编译错误）
  MT_CHECK(e.Run("USE b;").all_ok());
  MT_EQ(e.engine.current_db(), std::string("b"));
  const ScriptReport miss = e.Run("get * in t;");
  MT_CHECK(!miss.all_ok());
  MT_CHECK(miss.statements[0].status.code() == DbCode::kSqlError);
  MT_CHECK(!miss.statements[0].compile_errors.empty());

  // b 里建同名表，互不干扰
  MT_CHECK(e.Run("CREATE TABLE t(id INT, v VARCHAR(8));INSERT INTO t VALUES (2,'b');").all_ok());
  MT_EQ(RowsText(e.Run("get id, v in t;").statements[0].result), std::string("2|b"));

  // 切回 main：数据还是 main 的
  MT_CHECK(e.Run("USE main;").all_ok());
  MT_EQ(RowsText(e.Run("get id in t;").statements[0].result), std::string("1"));

  // 切换即落盘旧库：重开后（回到启动库 main）b 的数据仍在
  MT_CHECK(e.Reopen());
  MT_EQ(e.engine.current_db(), std::string("main"));
  MT_CHECK(e.Run("USE b;").all_ok());
  MT_EQ(RowsText(e.Run("get id, v in t;").statements[0].result), std::string("2|b"));
}

MT_TEST(多库_每库自包含) {
  Engine e("db_selfcontained");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE DATABASE b;USE b;").all_ok());
  MT_CHECK(e.Run("CREATE TABLE t(id INT, v VARCHAR(8));INSERT INTO t VALUES (7,'x');").all_ok());
  e.Close();

  // 只拷 b.db 一个文件到新目录即可作为独立库打开（单文件自包含）
  const std::string copy_dir = testutil::FreshDir("db_selfcontained_copy");
  std::error_code ec;
  std::filesystem::copy_file(e.cfg.data_dir + "/b.db", copy_dir + "/main.db", ec);
  MT_CHECK(!ec);

  Engine e2("db_selfcontained_open");
  e2.Close();
  e2.cfg.data_dir = copy_dir;
  MT_CHECK(e2.Reopen());
  MT_EQ(RowsText(e2.Run("get id, v in t;").statements[0].result), std::string("7|x"));
}

MT_TEST(多库_锁不串库) {
  Engine e("db_lock_scope");
  MT_CHECK(e.opened);

  // 锁资源名带库前缀 <db>.<table>：两个库的同名表互不冲突（先到先得，都立即成功）
  MT_CHECK(e.engine.locks().Acquire(11, "a.t", LockMode::kExclusive).ok());
  MT_CHECK(e.engine.locks().Acquire(12, "b.t", LockMode::kExclusive).ok());

  // 同库内仍然互斥：12 申请 a.t 的 S 锁须阻塞，由后台线程释放后醒来
  const auto t0 = std::chrono::steady_clock::now();
  std::atomic<bool> released{false};
  std::thread th([&e, &released] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    (void)e.engine.locks().ReleaseAll(11);
    released = true;
  });
  const DbStatus s3 = e.engine.locks().Acquire(12, "a.t", LockMode::kShared);
  th.join();
  const auto waited = std::chrono::steady_clock::now() - t0;
  MT_CHECK(s3.ok());
  MT_CHECK(released.load());
  MT_CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count() >= 100);
  (void)e.engine.locks().ReleaseAll(12);
}

MT_TEST(多库_事务中禁止切换) {
  Engine e("db_txn_guard");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE DATABASE b;").all_ok());
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO t VALUES (1);").all_ok());
  MT_CHECK(e.session().in_transaction());

  // 事务中 USE / CREATE / DROP DATABASE → DB-513（SHOW 只读，放行）
  MT_CHECK(e.Run("USE b;").statements[0].status.code() == DbCode::kDatabaseTxnActive);
  MT_CHECK(e.Run("CREATE DATABASE c;").statements[0].status.code() == DbCode::kDatabaseTxnActive);
  MT_CHECK(e.Run("DROP DATABASE b;").statements[0].status.code() == DbCode::kDatabaseTxnActive);
  MT_CHECK(e.Run("SHOW DATABASES;").all_ok());

  // 事务仍可用，提交后即可切换
  MT_CHECK(e.Run("COMMIT;").all_ok());
  MT_CHECK(e.Run("USE b;").all_ok());
  MT_EQ(e.engine.current_db(), std::string("b"));
}

MT_TEST(多库_非法名与缺失库) {
  Engine e("db_invalid");
  MT_CHECK(e.opened);

  // 非法名 / 缺失库 / 保留字 → DB-514
  MT_CHECK(e.Run("CREATE DATABASE 1abc;").statements[0].status.code() == DbCode::kDatabaseError);
  MT_CHECK(e.Run("CREATE DATABASE SELECT;").statements[0].status.code() == DbCode::kDatabaseError);
  MT_CHECK(e.Run("USE school;").statements[0].status.code() == DbCode::kDatabaseError);
  MT_CHECK(e.Run("DROP DATABASE school;").statements[0].status.code() == DbCode::kDatabaseError);

  // "my db" 带空格 → 不是合法的数据库控制语句，交给编译器报标准语法错（不误吞）
  MT_CHECK(e.Run("CREATE DATABASE my db;").statements[0].status.code() == DbCode::kSqlError);

  // CREATE TABLE 不被误吞（识别必须匹配前两词）
  MT_CHECK(e.Run("CREATE TABLE database_x(id INT);").all_ok());
}

MT_TEST(多库_DROP_DATABASE_软删除) {
  Engine e("db_drop");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE DATABASE b;USE b;CREATE TABLE t(id INT);").all_ok());
  MT_CHECK(e.Run("USE main;").all_ok());

  // 当前库 / 启动库不可删 → DB-515
  MT_CHECK(e.Run("DROP DATABASE main;").statements[0].status.code() == DbCode::kDatabaseProtected);
  MT_CHECK(e.Run("USE b;").all_ok());
  MT_CHECK(e.Run("DROP DATABASE b;").statements[0].status.code() == DbCode::kDatabaseProtected);

  // 非当前库可删：软删除 = 改名 <db>.db.dropped-<时间戳>
  MT_CHECK(e.Run("USE main;").all_ok());
  MT_CHECK(e.Run("DROP DATABASE b;").all_ok());
  MT_CHECK(!std::filesystem::exists(e.cfg.data_dir + "/b.db"));
  bool found_dropped = false;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(e.cfg.data_dir, ec)) {
    if (entry.path().filename().string().rfind("b.db.dropped-", 0) == 0) {
      found_dropped = true;
    }
  }
  MT_CHECK(found_dropped);
  MT_CHECK(e.Run("USE b;").statements[0].status.code() == DbCode::kDatabaseError);

  // SHOW DATABASES 不再列出 b
  MT_CHECK(!Contains(DbNames(e.Run("SHOW DATABASES;")), "b"));

  // 恢复：把留档文件改回原名即可重新 USE，数据还在
  for (const auto& entry : std::filesystem::directory_iterator(e.cfg.data_dir, ec)) {
    if (entry.path().filename().string().rfind("b.db.dropped-", 0) == 0) {
      std::filesystem::rename(entry.path(), e.cfg.data_dir + "/b.db", ec);
    }
  }
  MT_CHECK(!ec);
  MT_CHECK(e.Run("USE b;").all_ok());
  MT_CHECK(e.Run("get * in t;").all_ok());
}

MT_TEST(多库_旧布局迁移) {
  // 旧版数据文件叫 cella.db：新版首次打开自动改名为 main.db
  const std::string dir = testutil::FreshDir("db_legacy_layout");
  Engine seed("db_legacy_source");
  MT_CHECK(seed.opened);
  MT_CHECK(seed.Run("CREATE TABLE t(id INT);INSERT INTO t VALUES (5);").all_ok());
  seed.Close();

  std::error_code ec;
  std::filesystem::copy_file(seed.cfg.data_dir + "/main.db", dir + "/cella.db", ec);
  MT_CHECK(!ec);

  Engine e("db_legacy_open");
  e.Close();
  e.cfg.data_dir = dir;
  MT_CHECK(e.Reopen());
  MT_CHECK(std::filesystem::exists(dir + "/main.db"));  // 已迁移
  MT_CHECK(!std::filesystem::exists(dir + "/cella.db"));
  MT_EQ(e.engine.current_db(), std::string("main"));
  MT_EQ(RowsText(e.Run("get id in t;").statements[0].result), std::string("5"));
}
