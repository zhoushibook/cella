// test_index.cpp —— 二级索引 DDL 与元数据（P1.2）。
//
// 覆盖：
//   * CREATE [UNIQUE] INDEX / DROP INDEX 的正常路径与错误码
//   * 索引元数据落在独立系统表 cella_index（不污染 cella_catalog 行格式）
//   * 重启后索引元数据随目录一并恢复
//   * DROP TABLE 级联清理该表的索引元数据
//   * 唯一索引在已存在重复值时报错、在无重复值时成功
//   * SHOW INDEXES [IN table]
//
// 注意（P1.4 起）：带主键的表在建表时会自动获得 <table>_pk 唯一索引，
// 因此 emp（id INT PRIMARY KEY）在 SHOW INDEXES 里会**恒有一条** _pk 记录。
// 下面各用例的数量断言都已把这个自动索引算进去。
#include <memory>
#include <string>

#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {
const char* kBase = "CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(20), dept VARCHAR(10));";

// P1.4：主键自动索引的名字规则（与 Executor::PrimaryIndexName 保持一致）
std::string PkIndex(const std::string& table) { return table + "_pk"; }

// SHOW INDEXES 结果里出现过的索引名（按行拆，取每行首列）
bool HasIndex(const std::string& rows_text, const std::string& name) {
  return rows_text.find(name + "|") != std::string::npos;
}
}  // namespace

MT_TEST(索引_建表后可建普通索引并见于SHOW) {
  Engine e("ix_create");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "INSERT INTO emp VALUES (1,'a','x');"
                         "INSERT INTO emp VALUES (2,'b','y');"
                         "CREATE INDEX idx_name ON emp (name);");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  // emp 的自动主键索引 + idx_name
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 2);
  // 前 4 列固定：索引名 | 表名 | 列名 | 唯一标志（第 5 列是根页号，随分配变化）
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), PkIndex("emp")));
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), "idx_name"));
}

MT_TEST(索引_SHOW_INDEXES_IN_按表过滤) {
  Engine e("ix_show_in");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE TABLE d (id INT, v VARCHAR(8));"
                         "CREATE INDEX idx_a ON emp (name);"
                         "CREATE INDEX idx_b ON d (v);");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  // emp_pk + idx_a + idx_b（表 d 无主键 → 无自动索引）
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 3);

  r = e.Run("SHOW INDEXES IN emp;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 2);
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), "idx_a"));
}

MT_TEST(索引_SHOW_INDEXES_表不存在报DB502) {
  Engine e("ix_show_missing");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run("SHOW INDEXES IN nope;");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[0].status.code() == DbCode::kTableNotFound);
}

// 重名索引由语义层（SEM-315）拦下 → 对外统一是 DB-501。
// 语义层能拦是因为编译器目录视图里带着索引元数据（ToCompilerCatalog）。
MT_TEST(索引_重复建同名索引被语义层拦下) {
  Engine e("ix_dup_name");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE INDEX idx_name ON emp (name);"
                         "CREATE INDEX IDX_NAME ON emp (name);");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[2].status.code() == DbCode::kSqlError);
  MT_CHECK(r.statements[2].status.message().find("SEM-315") != std::string::npos);
  // 失败后目录里只有自动主键索引 + idx_name
  r = e.Run("SHOW INDEXES IN emp;");
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 2);
}

MT_TEST(索引_建在不存在的列上报SEM303) {
  Engine e("ix_bad_col");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) + "CREATE INDEX idx_x ON emp (nope);");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[1].status.message().find("SEM-303") != std::string::npos);
}

MT_TEST(索引_DROP不存在的索引报SEM316) {
  Engine e("ix_drop_missing");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run("DROP INDEX no_such_idx;");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[0].status.message().find("SEM-316") != std::string::npos);
}

MT_TEST(索引_DROP后SHOW不再出现) {
  Engine e("ix_drop");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) + "CREATE INDEX idx_name ON emp (name);");
  MT_CHECK(r.all_ok());
  r = e.Run("DROP INDEX idx_name;");
  MT_CHECK(r.all_ok());
  r = e.Run("SHOW INDEXES IN emp;");
  MT_CHECK(r.all_ok());
  // 只剩 P1.4 的自动主键索引
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 1);
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), PkIndex("emp")));
}

MT_TEST(索引_唯一索引在已有重复值时建立失败) {
  Engine e("ix_unique_dup");
  MT_CHECK(e.opened);

  // dept 有重复值 → 唯一索引必须被拒绝，且不留下半成品元数据
  const ScriptReport r = e.Run(std::string(kBase) +
                               "INSERT INTO emp VALUES (1,'a','eng');"
                               "INSERT INTO emp VALUES (2,'b','eng');"
                               "CREATE UNIQUE INDEX uq_dept ON emp (dept);");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[3].status.code() == DbCode::kIndexExists);

  const ScriptReport s = e.Run("SHOW INDEXES IN emp;");
  MT_CHECK(s.all_ok());
  // 自动主键索引仍在（唯一索引建立失败不影响它），uq_dept 未被登记
  MT_EQ(static_cast<int>(s.statements[0].result.rows.size()), 1);
  MT_CHECK(HasIndex(RowsText(s.statements[0].result), PkIndex("emp")));
}

MT_TEST(索引_唯一索引在无重复值时建立成功) {
  Engine e("ix_unique_ok");
  MT_CHECK(e.opened);

  const ScriptReport r = e.Run(std::string(kBase) +
                               "INSERT INTO emp VALUES (1,'a','eng');"
                               "INSERT INTO emp VALUES (2,'b','ops');"
                               "CREATE UNIQUE INDEX uq_dept ON emp (dept);");
  MT_CHECK(r.all_ok());

  const ScriptReport s = e.Run("SHOW INDEXES IN emp;");
  MT_CHECK(s.all_ok());
  MT_EQ(static_cast<int>(s.statements[0].result.rows.size()), 2);
  MT_CHECK(HasIndex(RowsText(s.statements[0].result), "uq_dept"));
}

MT_TEST(索引_重启后元数据恢复) {
  Engine e("ix_persist");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) + "CREATE UNIQUE INDEX uq_name ON emp (name);");
  MT_CHECK(r.all_ok());
  MT_CHECK(e.Reopen());

  r = e.Run("SHOW INDEXES IN emp;");
  MT_CHECK(r.all_ok());
  // 自动主键索引 + uq_name（两者都要从 cella_index 恢复）
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 2);
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), "uq_name"));
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), PkIndex("emp")));
}

MT_TEST(索引_DROP_TABLE级联清理索引) {
  Engine e("ix_cascade");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE INDEX idx_a ON emp (name);"
                         "CREATE INDEX idx_b ON emp (dept);");
  MT_CHECK(r.all_ok());
  r = e.Run("SHOW INDEXES IN emp;");
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 3); // emp_pk + a + b

  r = e.Run("DROP TABLE emp;");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);

  // 重启后仍然是 0（元数据行确实被持久化删除了）
  MT_CHECK(e.Reopen());
  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);
}

MT_TEST(索引_系统表禁止直接改) {
  Engine e("ix_sys_protect");
  MT_CHECK(e.opened);

  // cella_index 是受保护系统表：不能 INSERT / DROP。
  // 值类型要与列定义匹配（否则语义层先报 SEM-306 类型不匹配）。
  const ScriptReport r = e.Run(
      "INSERT INTO cella_index VALUES ('x','y','z',0,0,0);");
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements[0].status.code() == DbCode::kSystemTableProtected);

  const ScriptReport d = e.Run("DROP TABLE cella_index;");
  MT_CHECK(!d.all_ok());
  MT_CHECK(d.statements[0].status.code() == DbCode::kSystemTableProtected);
}

MT_TEST(索引_可以查询索引系统表) {
  Engine e("ix_sys_read");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) + "CREATE INDEX idx_name ON emp (name);");
  MT_CHECK(r.all_ok());

  // 只读查询系统表是放行的（与 cella_catalog 一致的策略）
  r = e.Run("GET * IN cella_index;");
  MT_CHECK(r.all_ok());
  // emp_pk（P1.4 自动）+ idx_name
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 2);
}

// SHOW INDEXES 必须带表头：客户端靠 columns 非空判定「这是查询」
// （QueryResult::IsQuery），缺表头会让 REST JSON 里 columns 为 null，
// Web 界面渲染不出网格。此用例防止该回归。
MT_TEST(索引_SHOW_INDEXES_必须带列元数据) {
  Engine e("ix_show_columns");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) + "CREATE INDEX idx_name ON emp (name);");
  MT_CHECK(r.all_ok());

  r = e.Run("SHOW INDEXES;");
  MT_CHECK(r.all_ok());
  const QueryResult& q = r.statements[0].result;
  MT_CHECK(q.IsQuery());
  MT_EQ(static_cast<int>(q.columns.size()), 5);
  MT_EQ(q.columns[0].name, std::string("index_name"));
  MT_EQ(q.columns[1].name, std::string("table_name"));
  MT_EQ(q.columns[2].name, std::string("column_name"));
  MT_EQ(q.columns[3].name, std::string("unique"));
  MT_EQ(q.columns[4].name, std::string("root_page_id"));
  // 每行的列数必须与表头一致
  MT_EQ(static_cast<int>(q.rows[0].size()), 5);
}

// ── 复合索引（多列 B+ 树）─────────────────────────────────────
//
// 覆盖：复合 CREATE INDEX 的元数据（逗号拼接列清单，行格式不变）、
// 唯一复合索引的冲突判定、重启后恢复、复合主键自动索引的键序。

MT_TEST(索引_复合索引建立与元数据) {
  Engine e("ix_composite_meta");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(std::string(kBase) +
                         "CREATE INDEX idx_dept_name ON emp (dept, name);");
  MT_CHECK(r.all_ok());

  // 元数据：列清单以逗号拼接落库（cella_index 行格式不变）
  const CatalogIndex *ix = e.engine.catalog().FindIndex("idx_dept_name");
  MT_CHECK(ix != nullptr);
  MT_CHECK(ix != nullptr && ix->columns.size() == 2u);
  MT_CHECK(ix != nullptr && ix->columns[0] == "dept");
  MT_CHECK(ix != nullptr && ix->columns[1] == "name");
  MT_CHECK(ix != nullptr && ix->column == "dept,name");
  MT_CHECK(ix != nullptr && !ix->unique);

  r = e.Run("SHOW INDEXES IN emp;");
  MT_CHECK(r.all_ok());
  // emp_pk + idx_dept_name
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 2);
  MT_CHECK(HasIndex(RowsText(r.statements[0].result), "idx_dept_name"));
}

MT_TEST(索引_复合唯一索引冲突与放行) {
  Engine e("ix_composite_unique");
  MT_CHECK(e.opened);

  // (building, room_no) 组合唯一：同组合拒绝，单列重复但组合不同放行
  ScriptReport r = e.Run(
      "CREATE TABLE room (id INT PRIMARY KEY, building VARCHAR(8), room_no VARCHAR(8));"
      "CREATE UNIQUE INDEX uq_loc ON room (building, room_no);"
      "INSERT INTO room VALUES (1,'A','101');"
      "INSERT INTO room VALUES (2,'A','102');");  // building 重复但组合不同 → OK
  MT_CHECK(r.all_ok());

  r = e.Run("INSERT INTO room VALUES (3,'A','101');");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kUniqueViolation));

  // 删除后组合键可重用（索引同步维护）
  MT_CHECK(e.Run("DELETE in room limit id = 1;").all_ok());
  MT_CHECK(e.Run("INSERT INTO room VALUES (4,'A','101');").all_ok());
}

MT_TEST(索引_复合唯一索引回填拒绝存量重复) {
  Engine e("ix_composite_backfill");
  MT_CHECK(e.opened);

  // 先插数据再建唯一复合索引：存在重复组合 → 建索引失败
  // 语句下标：0 建表 / 1 插入 / 2 插入（重复组合，无约束时合法）/ 3 建唯一索引
  ScriptReport r = e.Run(
      "CREATE TABLE t (a INT, b VARCHAR(8));"
      "INSERT INTO t VALUES (1,'x');"
      "INSERT INTO t VALUES (1,'x');"
      "CREATE UNIQUE INDEX uq_ab ON t (a, b);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[3].status.code()),
        static_cast<int>(DbCode::kIndexExists));

  // 改成普通索引则允许（非唯一索引不做查重）
  MT_CHECK(e.Run("CREATE INDEX idx_ab ON t (a, b);").all_ok());
}

MT_TEST(索引_复合索引重启恢复与维护) {
  Engine e("ix_composite_reopen");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(
      "CREATE TABLE ord (a INT, b VARCHAR(8), v INT);"
      "CREATE INDEX idx_ab ON ord (a, b);"
      "INSERT INTO ord VALUES (1,'x',10);"
      "INSERT INTO ord VALUES (1,'y',20);"
      "INSERT INTO ord VALUES (2,'x',30);");
  MT_CHECK(r.all_ok());

  // 全键等值走复合索引（两列都有等值条件 → 前缀扫描）
  r = e.Run("GET v IN ord limit a = 1 and b = 'y';");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("20"));

  // 重启：索引元数据与树一起恢复，等值查找仍正确
  e.Close();
  MT_CHECK(e.Reopen());
  r = e.Run("GET v IN ord limit a = 2 and b = 'x';");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("30"));
  // 更新后索引同步（删旧键 + 插新键）
  MT_CHECK(e.Run("UPDATE ord SET b = 'z' limit a = 1 and b = 'y';").all_ok());
  r = e.Run("GET v IN ord limit a = 1 and b = 'z';");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("20"));
}

MT_TEST(索引_复合主键自动索引按声明序) {
  Engine e("ix_composite_pk");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(
      "CREATE TABLE sc (sid INT, cid INT, grade FLOAT, PRIMARY KEY (sid, cid));"
      "INSERT INTO sc VALUES (1,10,88.0);"
      "INSERT INTO sc VALUES (1,11,90.0);");
  MT_CHECK(r.all_ok());

  // 自动索引存在、唯一、列序 = 声明序
  const CatalogIndex *ix = e.engine.catalog().FindIndex("sc_pk");
  MT_CHECK(ix != nullptr);
  MT_CHECK(ix != nullptr && ix->unique);
  MT_CHECK(ix != nullptr && ix->columns.size() == 2u);
  MT_CHECK(ix != nullptr && ix->columns[0] == "sid" && ix->columns[1] == "cid");

  // 组合重复 → 主键冲突（由复合索引判定）
  r = e.Run("INSERT INTO sc VALUES (1,10,50.0);");
  MT_CHECK(!r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].status.code()),
        static_cast<int>(DbCode::kPrimaryKeyViolation));

  // 全键等值命中复合主键索引
  r = e.Run("GET grade IN sc limit sid = 1 and cid = 11;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("90"));
}

MT_TEST(索引_复合索引缺前缀列退回全表扫描) {
  Engine e("ix_composite_prefix");
  MT_CHECK(e.opened);

  ScriptReport r = e.Run(
      "CREATE TABLE m (a INT, b INT, v INT);"
      "CREATE INDEX idx_ab ON m (a, b);"
      "INSERT INTO m VALUES (1,1,10);"
      "INSERT INTO m VALUES (1,2,20);"
      "INSERT INTO m VALUES (2,1,30);");
  MT_CHECK(r.all_ok());

  // 只约束第二列 b（非最左前缀）→ 不下推，交给 Filter（结果必须仍正确）
  r = e.Run("GET v IN m limit b = 1;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("10\n30"));
}
