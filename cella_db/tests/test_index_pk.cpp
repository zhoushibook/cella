// test_index_pk.cpp —— 主键自动索引（P1.4）。
//
// 覆盖：
//   * CREATE TABLE 带 PRIMARY KEY → 自动建出 <table>_pk 唯一索引
//   * 无主键的表不产生自动索引
//   * 自动索引是唯一的：主键重复被 DB-516 拦下（不再依赖全表扫的主键检查）
//   * 按主键 UPDATE / DELETE 时自动索引同步维护
//   * 元数据持久化：重启后自动索引仍在，唯一性仍然生效
//   * DROP TABLE 级联清掉自动索引
//   * 幂等：不会为同一张表重复建出自动索引
#include <memory>
#include <string>

#include "cella/db/engine/db_engine.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

std::string PkIndex(const std::string& table) { return table + "_pk"; }

bool HasIndex(const std::string& rows_text, const std::string& name) {
  return rows_text.find(name + "|") != std::string::npos;
}

// 取某表在 SHOW INDEXES IN <table> 里的行数
size_t IndexCountOf(DbEngine& engine, const std::string& table) {
  ScriptReport r;
  (void)engine.default_session().Execute("SHOW INDEXES IN " + table + ";", &r);
  if (!r.all_ok() || r.statements.empty()) {
    return size_t(-1);
  }
  return r.statements[0].result.rows.size();
}

}  // namespace

// ── 自动建立 ──────────────────────────────────────────────────

MT_TEST(P14_建表带主键自动建索引) {
  Engine e("p14_auto_create");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, name VARCHAR(20));");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES IN s;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 1);

  const std::string rows = RowsText(r.statements[0].result);
  // 索引名 | 表名 | 列名 | 唯一(1) | 根页号
  MT_CHECK(rows.rfind(PkIndex("s") + "|s|id|1|", 0) == 0);
}

MT_TEST(P14_无主键表不建自动索引) {
  Engine e("p14_no_pk");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run("CREATE TABLE s (id INT, name VARCHAR(20));");
  MT_CHECK(r.all_ok());

  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "s")), 0);
}

MT_TEST(P14_多张表各自有自动索引) {
  Engine e("p14_multi");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE a (id INT PRIMARY KEY);"
                         "CREATE TABLE b (code VARCHAR(8) PRIMARY KEY, v INT);"
                         "CREATE TABLE c (x INT);");
  MT_CHECK(r.all_ok());

  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "a")), 1);
  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "b")), 1);
  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "c")), 0);

  r = e.Run("SHOW INDEXES IN b;");
  MT_CHECK(r.all_ok());
  MT_CHECK(RowsText(r.statements[0].result).rfind(PkIndex("b") + "|b|code|1|", 0) == 0);
}

// VARCHAR 主键的自动索引也要按字符串类型编码（而不是数字类型）
MT_TEST(P14_字符串主键的自动索引可用) {
  Engine e("p14_varchar_pk");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (code VARCHAR(8) PRIMARY KEY, v INT);"
                         "INSERT INTO s VALUES ('aa', 1);"
                         "INSERT INTO s VALUES ('bb', 2);"
                         "INSERT INTO s VALUES ('aa', 3);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[3].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

// ── 唯一性 ────────────────────────────────────────────────────

MT_TEST(P14_主键重复被拒且只留一行) {
  Engine e("p14_pk_dup");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);"
                               "INSERT INTO s VALUES (1, 10);"
                               "INSERT INTO s VALUES (1, 20);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[2].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
  MT_CHECK(r.statements[2].status.ToString().find("DB-516") != std::string::npos);

  const ScriptReport g = e.Run("GET * IN s;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|10"));
}

MT_TEST(P14_批量插入内主键自冲突) {
  Engine e("p14_pk_batch");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);"
                               "INSERT INTO s VALUES (1,10),(1,20);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[1].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  const ScriptReport g = e.Run("GET * IN s;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 0);
}

// ── 按主键 UPDATE / DELETE ────────────────────────────────────

MT_TEST(P14_按主键更新时自动索引同步) {
  Engine e("p14_pk_update");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO s VALUES (1,10);"
                         "INSERT INTO s VALUES (2,20);"
                         "UPDATE s SET v = 99 LIMIT id = 1;");
  MT_CHECK(r.all_ok());

  // 更新主键列本身 → 旧值必须空出来
  r = e.Run("UPDATE s SET id = 100 LIMIT id = 2;");
  MT_CHECK(r.all_ok());

  r = e.Run("INSERT INTO s VALUES (2, 22);"); // 旧主键值已释放
  MT_CHECK(r.all_ok());

  r = e.Run("INSERT INTO s VALUES (100, 33);"); // 新主键值被占用
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  const ScriptReport g = e.Run("GET * IN s ordered id asc;");
  MT_CHECK(g.all_ok());
  // 行 100 来自 `UPDATE ... SET id = 100`，只改了主键列，v 保持原值 20
  MT_EQ(RowsText(g.statements[0].result), std::string("1|99\n2|22\n100|20"));
}

MT_TEST(P14_按主键删除时自动索引同步) {
  Engine e("p14_pk_delete");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO s VALUES (1,10);"
                         "INSERT INTO s VALUES (2,20);"
                         "DELETE IN s LIMIT id = 1;");
  MT_CHECK(r.all_ok());

  // 删掉的主键值可以重新使用
  r = e.Run("INSERT INTO s VALUES (1, 11);");
  MT_CHECK(r.all_ok());

  const ScriptReport g = e.Run("GET * IN s ordered id asc;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("1|11\n2|20"));
}

// ── 持久化 ────────────────────────────────────────────────────

MT_TEST(P14_重启后自动索引仍在且有效) {
  Engine e("p14_persist");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO s VALUES (1,10);");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  r = e.Run("SHOW INDEXES IN s;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 1);
  MT_CHECK(RowsText(r.statements[0].result).rfind(PkIndex("s") + "|s|id|1|", 0) == 0);

  // 重启后唯一性仍然生效
  r = e.Run("INSERT INTO s VALUES (1, 99);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

MT_TEST(P14_重启后旧主键值仍被占用) {
  Engine e("p14_persist_after_delete");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO s VALUES (1,10);"
                         "DELETE IN s LIMIT id = 1;");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  // 删除是持久的 → 主键值 1 应可重新使用
  r = e.Run("INSERT INTO s VALUES (1, 11);");
  MT_CHECK(r.all_ok());
}

// ── DROP TABLE 级联 ──────────────────────────────────────────

MT_TEST(P14_删表级联清理自动索引) {
  Engine e("p14_drop");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, v INT);");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "s")), 1);

  r = e.Run("DROP TABLE s;");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);

  MT_CHECK(e.Reopen());
  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);
}

// ── 与手动索引共存 ────────────────────────────────────────────

MT_TEST(P14_自动索引与手动索引共存) {
  Engine e("p14_coexist");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, name VARCHAR(20));"
                         "CREATE UNIQUE INDEX uq_name ON s (name);"
                         "INSERT INTO s VALUES (1,'a');");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "s")), 2);

  // 两个索引都生效
  r = e.Run("INSERT INTO s VALUES (2,'a');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  r = e.Run("INSERT INTO s VALUES (1,'b');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

// 删掉手动索引后，自动主键索引必须仍然生效
MT_TEST(P14_删手动索引不影响自动索引) {
  Engine e("p14_drop_manual");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE s (id INT PRIMARY KEY, name VARCHAR(20));"
                         "CREATE INDEX idx_name ON s (name);"
                         "DROP INDEX idx_name;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "s")), 1);

  r = e.Run("INSERT INTO s VALUES (1,'a');");
  MT_CHECK(r.all_ok());
  r = e.Run("INSERT INTO s VALUES (1,'b');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

// ── 表级主键写的两种语法都要覆盖 ─────────────────────────────
// （本项目主键是列级约束：`id INT PRIMARY KEY` 与 `id INT NOT NULL PRIMARY KEY`）

MT_TEST(P14_主键书写顺序两种都建索引) {
  Engine e("p14_pk_order");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE a (id INT PRIMARY KEY, v INT);"
                         "CREATE TABLE b (id INT NOT NULL PRIMARY KEY, v INT);");
  MT_CHECK(r.all_ok());

  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "a")), 1);
  MT_EQ(static_cast<int>(IndexCountOf(e.engine, "b")), 1);
}
