// test_alter.cpp —— DDL 演进能力（P5）：ALTER TABLE / TRUNCATE TABLE。
//
// 覆盖（对应 GAP_ANALYSIS 的 P5.1–P5.5）：
//   * P5.1 ADD COLUMN：老行补 NULL，新行可写；NOT NULL 新列在非空表上被拒
//   * P5.2 DROP COLUMN：行布局重写正确；涉及该列的索引级联删除
//   * P5.3 RENAME TO / RENAME COLUMN：目录与索引元数据同步（含 <表>_pk 改名）
//   * P5.4 ADD / DROP PRIMARY KEY：校验已有数据（非空 + 唯一），约束随之生效/失效
//   * P5.5 TRUNCATE：清空数据但保留结构；比 DELETE 快（走重建而非逐行删）
//   * 重建后索引仍然可用（行定位变了 → 索引必须整体重建）
//   * 重启持久化：结构、数据、索引元数据都要活过 Close/Open
#include <memory>
#include <string>
#include <vector>

#include "cella/db/engine/db_engine.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

// SHOW INDEXES IN <table> 的结果文本（一行一个索引：名|表|列|唯一|根页号）
std::string IndexesText(Engine& e, const std::string& table) {
  ScriptReport r;
  (void)e.session().Execute("SHOW INDEXES IN " + table + ";", &r);
  if (!r.all_ok() || r.statements.empty()) {
    return std::string();
  }
  return RowsText(r.statements[0].result);
}

bool HasIndex(const std::string& rows, const std::string& prefix) {
  return rows.find(prefix) != std::string::npos;
}

size_t RowCount(Engine& e, const std::string& table) {
  ScriptReport r;
  (void)e.session().Execute("GET rowid IN " + table + ";", &r);
  if (!r.all_ok() || r.statements.empty()) {
    return size_t(-1);
  }
  return r.statements[0].result.rows.size();
}

}  // namespace

// ── P5.1 ADD COLUMN ───────────────────────────────────────────

MT_TEST(P51_加列后老行补NULL且新行可写) {
  Engine e("p5_add_col");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL);"
                         "INSERT INTO stu VALUES (1, 'amy');"
                         "INSERT INTO stu VALUES (2, 'bob');");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE stu ADD COLUMN age INT;");
  MT_CHECK(r.all_ok());

  // 老行读出来必须有这一列，值是 NULL
  r = e.Run("GET id, name, age IN stu ORDERED id ASC;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|name|age"));
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy|NULL\n2|bob|NULL"));

  // 新插入的行可以带值
  r = e.Run("INSERT INTO stu VALUES (3, 'cid', 30);");
  MT_CHECK(r.all_ok());
  r = e.Run("GET * IN stu ORDERED id ASC;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy|NULL\n2|bob|NULL\n3|cid|30"));

  // NULL 是真的 NULL（能命中 IS NULL）
  r = e.Run("GET id IN stu LIMIT age IS NULL ORDERED id ASC;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("1\n2"));
}

MT_TEST(P51_非空加列在非空表上被拒) {
  Engine e("p5_add_col_notnull");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "INSERT INTO stu VALUES (1, 'amy');");
  MT_CHECK(r.all_ok());

  // 已有数据无法为老行补出 NOT NULL 值 → 拒绝，而不是偷偷写 NULL
  r = e.Run("ALTER TABLE stu ADD COLUMN age INT NOT NULL;");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kNotNullViolation));

  // 结构必须保持未变（列没加上）
  r = e.Run("GET * IN stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|name"));

  // 空表上则允许
  r = e.Run("CREATE TABLE empty_t (id INT);"
            "ALTER TABLE empty_t ADD COLUMN age INT NOT NULL;"
            "INSERT INTO empty_t VALUES (1, 5);");
  MT_CHECK(r.all_ok());
  r = e.Run("GET * IN empty_t;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("1|5"));
}

MT_TEST(P51_加列不破坏已有二级索引) {
  Engine e("p5_add_col_keep_index");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "CREATE INDEX idx_name ON stu (name);"
                         "INSERT INTO stu VALUES (1,'amy');"
                         "INSERT INTO stu VALUES (2,'bob');");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE stu ADD COLUMN age INT;");
  MT_CHECK(r.all_ok());

  // 行定位变了，索引必须被重建 → 唯一性/查找仍然有效
  r = e.Run("INSERT INTO stu VALUES (3,'amy',1);");  // 非唯一索引，允许
  MT_CHECK(r.all_ok());
  r = e.Run("INSERT INTO stu VALUES (1,'zed',2);");  // 主键仍在
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  const std::string ix = IndexesText(e, "stu");
  MT_CHECK(HasIndex(ix, "stu_pk|stu|id|1|"));
  MT_CHECK(HasIndex(ix, "idx_name|stu|name|0|"));
}

// ── P5.2 DROP COLUMN ──────────────────────────────────────────

MT_TEST(P52_删列后行布局正确) {
  Engine e("p5_drop_col");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20), age INT);"
                         "INSERT INTO stu VALUES (1,'amy',20);"
                         "INSERT INTO stu VALUES (2,'bob',30);");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE stu DROP COLUMN age;");
  MT_CHECK(r.all_ok());

  r = e.Run("GET * IN stu ORDERED id ASC;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|name"));
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy\n2|bob"));

  // 删掉的列不能再被引用
  r = e.Run("GET age IN stu;");
  MT_CHECK(!r.all_ok());
}

MT_TEST(P52_删列级联删除相关索引但保留无关索引) {
  Engine e("p5_drop_col_index");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20), age INT);"
                         "CREATE INDEX idx_name ON stu (name);"
                         "CREATE INDEX idx_age ON stu (age);");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE stu DROP COLUMN age;");
  MT_CHECK(r.all_ok());

  const std::string ix = IndexesText(e, "stu");
  MT_CHECK(HasIndex(ix, "stu_pk|stu|id|1|"));
  MT_CHECK(HasIndex(ix, "idx_name|stu|name|0|"));
  MT_CHECK(!HasIndex(ix, "idx_age"));

  // 剩余索引仍被维护
  r = e.Run("INSERT INTO stu VALUES (2,'bob');");
  MT_CHECK(r.all_ok());
  r = e.Run("INSERT INTO stu VALUES (2,'x');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

MT_TEST(P52_主键列不可直接删且最后一列不可删) {
  Engine e("p5_drop_col_guard");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "ALTER TABLE stu DROP COLUMN id;");  // 主键列 → 先删主键
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[1].status.code()), static_cast<int>(DbCode::kSqlError));

  // 表至少要留一列
  r = e.Run("CREATE TABLE solo (a INT);ALTER TABLE solo DROP COLUMN a;");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[1].status.code()), static_cast<int>(DbCode::kSqlError));

  // 删到只剩主键列是允许的（表仍有一列）
  r = e.Run("ALTER TABLE stu DROP COLUMN name;"
            "INSERT INTO stu VALUES (7);");
  MT_CHECK(r.all_ok());
  r = e.Run("GET * IN stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id"));
}

// ── P5.3 RENAME ───────────────────────────────────────────────

MT_TEST(P53_列改名后数据与索引同步) {
  Engine e("p5_rename_col");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "CREATE UNIQUE INDEX uq_name ON stu (name);"
                         "INSERT INTO stu VALUES (1,'amy');");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE stu RENAME COLUMN name TO sname;");
  MT_CHECK(r.all_ok());

  r = e.Run("GET * IN stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|sname"));
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy"));

  // 唯一索引的元数据列名要跟着改，唯一性仍然生效
  const std::string ix = IndexesText(e, "stu");
  MT_CHECK(HasIndex(ix, "uq_name|stu|sname|1|"));
  r = e.Run("INSERT INTO stu VALUES (2,'amy');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  r = e.Run("GET sname IN stu;");
  MT_CHECK(r.all_ok());
  r = e.Run("GET name IN stu;");
  MT_CHECK(!r.all_ok());
}

MT_TEST(P53_表改名后目录与主键索引同步) {
  Engine e("p5_rename_table");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "INSERT INTO stu VALUES (1,'amy');");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE stu RENAME TO student;");
  MT_CHECK(r.all_ok());

  r = e.Run("GET * IN student;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy"));

  r = e.Run("GET * IN stu;");
  MT_CHECK(!r.all_ok());  // 旧名已经不存在

  // <表>_pk 也要跟着改名，否则新表会被认为「没有主键」
  const std::string ix = IndexesText(e, "student");
  MT_CHECK(HasIndex(ix, "student_pk|student|id|1|"));
  MT_CHECK(!HasIndex(ix, "stu_pk"));

  r = e.Run("INSERT INTO student VALUES (1,'x');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  // 改成已存在的表名要拒绝（语义层先拦，报 DB-501 + SEM-302）
  r = e.Run("CREATE TABLE other (x INT);ALTER TABLE student RENAME TO other;");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[1].status.ToString().find("已存在") != std::string::npos);
  MT_EQ(static_cast<int>(r.statements[1].status.code()), static_cast<int>(DbCode::kSqlError));
}

// ── P5.4 ADD / DROP PRIMARY KEY ───────────────────────────────

MT_TEST(P54_加主键校验已有数据的唯一性) {
  Engine e("p5_add_pk_dup");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE t (code INT, v INT);"
                         "INSERT INTO t VALUES (1, 10);"
                         "INSERT INTO t VALUES (1, 20);");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE t ADD PRIMARY KEY (code);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  // 失败不能留下半个主键
  r = e.Run("SHOW INDEXES IN t;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);

  // 去掉重复值后可以加
  r = e.Run("DELETE IN t LIMIT code = 1 AND v = 20;");
  MT_CHECK(r.all_ok());
  r = e.Run("ALTER TABLE t ADD PRIMARY KEY (code);");
  MT_CHECK(r.all_ok());

  const std::string ix = IndexesText(e, "t");
  MT_CHECK(HasIndex(ix, "t_pk|t|code|1|"));

  r = e.Run("INSERT INTO t VALUES (1, 99);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

MT_TEST(P54_加主键拒绝含NULL的列) {
  Engine e("p5_add_pk_null");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE t (code INT, v INT);"
                         "INSERT INTO t VALUES (NULL, 10);");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE t ADD PRIMARY KEY (code);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kNotNullViolation));
}

MT_TEST(P54_删主键后唯一性不再生效) {
  Engine e("p5_drop_pk");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE t (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO t VALUES (1, 10);");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE t DROP PRIMARY KEY;");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES IN t;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);

  // 主键索引没了 → 重复值可以写进去（NOT NULL 仍保留）
  r = e.Run("INSERT INTO t VALUES (1, 20);");
  MT_CHECK(r.all_ok());
  r = e.Run("GET * IN t ORDERED v ASC;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("1|10\n1|20"));

  // NOT NULL 仍保留（MySQL 语义：删主键不放宽空值）；NULL 字面量在编译期就被拦
  r = e.Run("INSERT INTO t VALUES (NULL, 30);");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[0].status.ToString().find("NOT NULL") != std::string::npos);
  MT_EQ(static_cast<int>(r.statements[0].status.code()), static_cast<int>(DbCode::kSqlError));

  // 没有主键时再删一次要报错
  r = e.Run("ALTER TABLE t DROP PRIMARY KEY;");
  MT_CHECK(!r.all_ok());
}

MT_TEST(P54_先删主键再删原主键列) {
  Engine e("p5_drop_pk_then_col");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE t (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO t VALUES (1, 10);");
  MT_CHECK(r.all_ok());

  r = e.Run("ALTER TABLE t DROP COLUMN id;");  // 主键列，拒绝
  MT_CHECK(!r.all_ok());

  r = e.Run("ALTER TABLE t DROP PRIMARY KEY;"
            "ALTER TABLE t DROP COLUMN id;");
  MT_CHECK(r.all_ok());

  r = e.Run("GET * IN t;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("v"));
  MT_EQ(RowsText(r.statements[0].result), std::string("10"));
}

// ── P5.5 TRUNCATE ─────────────────────────────────────────────

MT_TEST(P55_清空数据但保留结构) {
  Engine e("p5_truncate");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "CREATE INDEX idx_name ON stu (name);"
                         "INSERT INTO stu VALUES (1,'amy');"
                         "INSERT INTO stu VALUES (2,'bob');");
  MT_CHECK(r.all_ok());
  MT_EQ(RowCount(e, "stu"), size_t(2));

  r = e.Run("TRUNCATE TABLE stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowCount(e, "stu"), size_t(0));

  // 结构与索引定义都保留
  r = e.Run("GET * IN stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|name"));
  const std::string ix = IndexesText(e, "stu");
  MT_CHECK(HasIndex(ix, "stu_pk|stu|id|1|"));
  MT_CHECK(HasIndex(ix, "idx_name|stu|name|0|"));

  // 清空后索引与约束仍然可用（旧键必须已被清掉）
  r = e.Run("INSERT INTO stu VALUES (1,'amy');");
  MT_CHECK(r.all_ok());
  r = e.Run("INSERT INTO stu VALUES (1,'bob');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
  r = e.Run("GET * IN stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy"));
}

MT_TEST(P55_系统表禁止清空) {
  Engine e("p5_truncate_sys");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run("TRUNCATE TABLE cella_catalog;");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kSystemTableProtected));
}

// ── 持久化 ────────────────────────────────────────────────────

MT_TEST(P5_改表结果重启后仍然有效) {
  Engine e("p5_persist");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));"
                         "CREATE INDEX idx_name ON stu (name);"
                         "INSERT INTO stu VALUES (1,'amy');"
                         "ALTER TABLE stu ADD COLUMN age INT;"
                         "ALTER TABLE stu RENAME COLUMN name TO sname;"
                         "ALTER TABLE stu RENAME TO student;");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  r = e.Run("GET * IN student;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|sname|age"));
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy|NULL"));

  const std::string ix = IndexesText(e, "student");
  MT_CHECK(HasIndex(ix, "student_pk|student|id|1|"));
  MT_CHECK(HasIndex(ix, "idx_name|student|sname|0|"));

  // 索引仍被维护（进程重启后 <表>_pk 必须仍然唯一）
  r = e.Run("INSERT INTO student VALUES (1,'x',1);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));
}

MT_TEST(P5_清空结果重启后仍然有效) {
  Engine e("p5_persist_truncate");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, v INT);"
                         "INSERT INTO stu VALUES (1,10);"
                         "TRUNCATE TABLE stu;");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  MT_EQ(RowCount(e, "stu"), size_t(0));
  r = e.Run("INSERT INTO stu VALUES (1,11);");  // 旧键已释放
  MT_CHECK(r.all_ok());
}

MT_TEST(P5_删列结果重启后行布局正确) {
  Engine e("p5_persist_drop_col");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run("CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20), age INT);"
                         "INSERT INTO stu VALUES (1,'amy',20);"
                         "ALTER TABLE stu DROP COLUMN age;");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  r = e.Run("GET * IN stu;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("id|name"));
  MT_EQ(RowsText(r.statements[0].result), std::string("1|amy"));
}
