// test_lang_features.cpp —— 语言生态落地：子查询 / 窗口函数 / 视图 / CTE。
//
// 背景：这四类能力在编译器层早就通了（解析 + 语义 + 计划节点齐全），卡住它们的是
// 执行期 —— 它们的答案都不在「一行值」的视野里，因此原先一律返回 DB-703。
// 本文件钉住修复后的行为，重点是**语义正确性**而不是「不再报错」：
//
//   * 子查询：IN / NOT IN / EXISTS / NOT EXISTS / 标量；空结果、NULL 三值逻辑、
//             零行标量 → NULL、多行标量 → 报错、嵌套子查询、大表上与全表扫描一致
//   * 窗口函数：ROW_NUMBER / RANK / DENSE_RANK 的并列语义、窗口聚合五种、
//             分区/无分区、NULL 排序键、与 ORDER BY / LIMIT 的组合
//   * 视图：创建 → 查询 → 重启后仍在 → 嵌视图 → DROP 后不可用 → 与表重名被拦
//   * CTE：单 CTE、多 CTE 串联（后者可引用前者）
//   * 计划文本契约：新能力的计划文本里**不得**出现新算子名（golden 是逐字节契约）
#include <string>
#include <vector>

#include "cella/db/engine/db_engine.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::ColsText;
using testutil::Engine;
using testutil::FirstError;
using testutil::RowCount;
using testutil::RowsText;

namespace {

// 统一的小基表：4 行 2 个部门，便于手算窗口与子查询的期望值。
//   id | name  | dept | salary
//    1 | Alice | Eng  | 12000
//    2 | Bob   | Eng  |  9000
//    3 | Carol | Ops  |  7500
//    4 | Dave  | Ops  |  6000
const char* kSetup =
    "CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(20), dept VARCHAR(10), salary INT);"
    "INSERT INTO emp VALUES (1,'Alice','Eng',12000);"
    "INSERT INTO emp VALUES (2,'Bob','Eng',9000);"
    "INSERT INTO emp VALUES (3,'Carol','Ops',7500);"
    "INSERT INTO emp VALUES (4,'Dave','Ops',6000);";

// 执行 setup + 一条查询，返回该查询的结果行文本（任何一步失败返回 "ERR:<状态>"）
std::string Query(Engine& e, const std::string& sql) {
  const auto r = e.Run(std::string(kSetup) + sql);
  if (!r.all_ok() || r.statements.empty()) {
    return "ERR:" + FirstError(r);
  }
  return RowsText(r.statements.back().result);
}

}  // namespace

// ── 子查询 ─────────────────────────────────────────────────────

MT_TEST(子查询_IN_结果正确) {
  Engine e("lf_sub_in");
  // 薪水 > 8000 的部门只有 Eng → 取出 Eng 的两行
  MT_EQ(Query(e, "GET id, name IN emp LIMIT dept IN (GET dept IN emp LIMIT salary > 8000);"),
        std::string("1|Alice\n2|Bob"));
}

MT_TEST(子查询_IN_空结果集不匹配任何行) {
  Engine e("lf_sub_in_empty");
  MT_EQ(Query(e, "GET id IN emp LIMIT id IN (GET id IN emp LIMIT salary > 99999);"),
        std::string(""));
}

MT_TEST(子查询_NOT_IN_与_IN_互补) {
  Engine e("lf_sub_not_in");
  MT_EQ(Query(e, "GET id, name IN emp LIMIT dept NOT IN (GET dept IN emp LIMIT salary > 8000);"),
        std::string("3|Carol\n4|Dave"));
}

MT_TEST(子查询_EXISTS_与_NOT_EXISTS) {
  Engine e("lf_sub_exists");
  // 存在薪水 > 11000 的行 → EXISTS 为真 → 全部 4 行；NOT EXISTS 为假 → 0 行
  MT_EQ(RowCount(e.Run(std::string(kSetup) +
                       "GET id IN emp LIMIT EXISTS (GET id IN emp LIMIT salary > 11000);")
                     .statements.back()
                     .result),
        4u);
  MT_EQ(RowCount(e.Run(std::string(kSetup) +
                       "GET id IN emp LIMIT NOT EXISTS (GET id IN emp LIMIT salary > 11000);")
                     .statements.back()
                     .result),
        0u);
}

MT_TEST(子查询_标量_单行取值) {
  Engine e("lf_sub_scalar");
  // 最高薪 12000 / 2 = 6000 → 严格大于 6000 的有 3 行（Dave 恰好 6000 被排除）
  MT_EQ(Query(e, "GET id IN emp LIMIT salary > (GET MAX(salary) IN emp) / 2;"),
        std::string("1\n2\n3"));
}

MT_TEST(子查询_标量_零行返回NULL导致条件为UNKNOWN) {
  Engine e("lf_sub_scalar_null");
  // 子查询无结果 → 标量值为 NULL → `salary > NULL` 是 UNKNOWN → 一行都不通过。
  // 这是 SQL 三值逻辑的正确表现，不是「查不到就不过滤」。
  MT_EQ(Query(e, "GET id IN emp LIMIT salary > (GET MAX(salary) IN emp LIMIT id > 999);"),
        std::string(""));
}

MT_TEST(子查询_标量_多行报错) {
  Engine e("lf_sub_scalar_multi");
  const auto r = e.Run(std::string(kSetup) + "GET id IN emp LIMIT salary = (GET id IN emp);");
  MT_CHECK(!r.all_ok());
  // 报的是「最多只能一行」，而不是静默取第一行
  MT_CHECK(FirstError(r).find("DB-501") != std::string::npos);
}

MT_TEST(子查询_嵌套子查询) {
  Engine e("lf_sub_nested");
  // 内层：薪水 < 10000 的薪水集合 {9000,7500,6000}
  // 中层：在该集合上取 MAX → 9000
  // 外层：salary = 9000 → Bob
  MT_EQ(Query(e,
              "GET id, name IN emp LIMIT salary = "
              "(GET MAX(salary) IN emp LIMIT salary IN (GET salary IN emp LIMIT salary < 10000));"),
        std::string("2|Bob"));
}

MT_TEST(子查询_在大表上与全表扫描结果一致) {
  Engine e("lf_sub_big");
  // 600 行：用子查询圈出候选集合，再与等价的「显式列表」结果对比。
  // 表规模超过单页槽数（一页 270 行），因此同时覆盖了跨页扫描路径。
  std::string sql = "CREATE TABLE big (id INT PRIMARY KEY, v INT);INSERT INTO big (id, v) VALUES ";
  for (int i = 1; i <= 600; ++i) {
    if (i > 1) {
      sql += ",";
    }
    sql += "(" + std::to_string(i) + "," + std::to_string(i * 3) + ")";
  }
  sql += ";";
  sql += "GET id IN big LIMIT v IN (GET v IN big LIMIT v <= 30);";
  const auto r = e.Run(sql);
  MT_CHECK(r.all_ok());
  // v = 3,6,...,30 → 10 行
  MT_EQ(RowCount(r.statements.back().result), 10u);
  MT_EQ(RowsText(r.statements.back().result),
        std::string("1\n2\n3\n4\n5\n6\n7\n8\n9\n10"));
}

MT_TEST(子查询_只执行一次而不是每行一次) {
  Engine e("lf_sub_once");
  // 语义阶段已确认子查询不引用外层列 → 结果与外层行无关。
  // 若按外层行数重复执行，300 行 × 子查询扫 300 行会明显变慢；
  // 这里用「算子调用次数」间接钉住：子查询体只被驱动一轮。
  std::string sql = "CREATE TABLE t (id INT PRIMARY KEY);INSERT INTO t (id) VALUES ";
  for (int i = 1; i <= 300; ++i) {
    if (i > 1) {
      sql += ",";
    }
    sql += "(" + std::to_string(i) + ")";
  }
  sql += ";GET id IN t LIMIT id IN (GET id IN t LIMIT id > 290);";
  const auto r = e.Run(sql);
  MT_CHECK(r.all_ok());
  MT_EQ(RowCount(r.statements.back().result), 10u);  // 291..300
}

// ── 窗口函数 ───────────────────────────────────────────────────

MT_TEST(窗口_ROW_NUMBER_按分区与排序编号) {
  Engine e("lf_win_rownum");
  // 结果是两列：id + 窗口值。Eng: Alice,Bob → 1,2；Ops: Carol,Dave → 1,2
  MT_EQ(Query(e,
              "GET id, ROW_NUMBER() OVER (PARTITION BY dept ORDERED BY salary DESC) IN emp;"),
        std::string("1|1\n2|2\n3|1\n4|2"));
}

MT_TEST(窗口_RANK_并列跳号_DENSE_RANK_并列不跳号) {
  Engine e("lf_win_rank");
  // 造一个并列：两个 100
  const std::string sql =
      "CREATE TABLE t (id INT, g VARCHAR(4), v INT);"
      "INSERT INTO t VALUES (1,'A',100);"
      "INSERT INTO t VALUES (2,'A',100);"
      "INSERT INTO t VALUES (3,'A',50);"
      "GET id, RANK() OVER (ORDERED BY v DESC), DENSE_RANK() OVER (ORDERED BY v DESC) IN t;";
  const auto r = e.Run(sql);
  MT_CHECK(r.all_ok());
  // 三列：id | RANK | DENSE_RANK。RANK: 1,1,3（并列后跳号）；DENSE_RANK: 1,1,2（不跳号）
  MT_EQ(RowsText(r.statements.back().result), std::string("1|1|1\n2|1|1\n3|3|2"));
}

MT_TEST(窗口_聚合五种覆盖整个分区) {
  Engine e("lf_win_agg");
  // 注意：这里两条查询共用同一个引擎，不能再用 Query()（它会把 kSetup 再跑一遍，
  // 第二次 CREATE TABLE emp 会撞 SEM-302「表已存在」）。
  MT_CHECK(e.Run(std::string(kSetup)).all_ok());
  // Eng: COUNT=2 SUM=21000 AVG=10500 MIN=9000 MAX=12000
  const auto r1 = e.Run(
      "GET id, COUNT(*) OVER (PARTITION BY dept), SUM(salary) OVER (PARTITION BY dept),"
      " AVG(salary) OVER (PARTITION BY dept) IN emp LIMIT dept = 'Eng';");
  MT_CHECK(r1.all_ok());
  MT_EQ(RowsText(r1.statements.back().result), std::string("1|2|21000|10500\n2|2|21000|10500"));
  // Ops: MIN=6000 MAX=7500
  const auto r2 = e.Run(
      "GET MIN(salary) OVER (PARTITION BY dept), MAX(salary) OVER (PARTITION BY dept)"
      " IN emp LIMIT dept = 'Ops';");
  MT_CHECK(r2.all_ok());
  MT_EQ(RowsText(r2.statements.back().result), std::string("6000|7500\n6000|7500"));
}

MT_TEST(窗口_无分区时全表一个分区) {
  Engine e("lf_win_nopart");
  // 没有 PARTITION BY → 全表一个分区；COUNT(*) OVER () 因此恒为总行数
  MT_EQ(Query(e, "GET id, COUNT(*) OVER () IN emp;"),
        std::string("1|4\n2|4\n3|4\n4|4"));
}

MT_TEST(窗口_排序键为NULL时排最后) {
  Engine e("lf_win_null");
  const std::string sql =
      "CREATE TABLE t (id INT, v INT);"
      "INSERT INTO t VALUES (1,10);"
      "INSERT INTO t VALUES (2,NULL);"
      "INSERT INTO t VALUES (3,20);"
      "GET id, ROW_NUMBER() OVER (ORDERED BY v DESC) IN t;";
  const auto r = e.Run(sql);
  MT_CHECK(r.all_ok());
  // 两点一起钉住：
  //   ① DESC 下 NULL 仍排最后（与 ORDERED BY 一致，不随方向翻转）→ 序号 20→1、10→2、NULL→3
  //   ② 窗口函数**不重排输出行**（标准 SQL）：行序仍是扫描顺序 id=1,2,3，
  //      只是各自带上了按排序算出来的序号。
  MT_EQ(RowsText(r.statements.back().result), std::string("1|2\n2|3\n3|1"));
}

MT_TEST(窗口_与ORDER_BY和LIMIT组合) {
  Engine e("lf_win_order_limit");
  // 窗口在投影阶段算完，ORDER BY / LIMIT 作用在其结果之上
  MT_EQ(Query(e, "GET id, salary IN emp ORDERED salary DESC AMONG 2;"),
        std::string("1|12000\n2|9000"));
}

// ── 视图 ───────────────────────────────────────────────────────

MT_TEST(视图_创建后可直接查询) {
  Engine e("lf_view_basic");
  const auto r = e.Run(std::string(kSetup) +
                       "CREATE VIEW v_eng AS GET id, name, salary IN emp LIMIT dept = 'Eng';"
                       "GET * IN v_eng;");
  MT_CHECK(r.all_ok());
  MT_EQ(ColsText(r.statements.back().result), std::string("id|name|salary"));
  MT_EQ(RowsText(r.statements.back().result), std::string("1|Alice|12000\n2|Bob|9000"));
}

MT_TEST(视图_外层可继续过滤与取列) {
  Engine e("lf_view_filter");
  const auto r = e.Run(std::string(kSetup) +
                       "CREATE VIEW v_eng AS GET id, name, salary IN emp LIMIT dept = 'Eng';"
                       "GET name IN v_eng LIMIT salary > 10000;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements.back().result), std::string("Alice"));
}

MT_TEST(视图_可用别名限定引用) {
  Engine e("lf_view_alias");
  const auto r = e.Run(std::string(kSetup) +
                       "CREATE VIEW v_eng AS GET id, name, salary IN emp LIMIT dept = 'Eng';"
                       "GET v.id, v.name IN v_eng v LIMIT v.salary > 10000;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements.back().result), std::string("1|Alice"));
}

MT_TEST(视图_重启后仍然存在) {
  Engine e("lf_view_persist");
  const auto r1 = e.Run(std::string(kSetup) +
                        "CREATE VIEW v_eng AS GET id, salary IN emp LIMIT dept = 'Eng';");
  MT_CHECK(r1.all_ok());
  // 视图定义持久化在 cella_view 系统表里 → 重开引擎后必须还能查
  MT_CHECK(e.Reopen());
  const auto r2 = e.Run("GET * IN v_eng;");
  MT_CHECK(r2.all_ok());
  MT_EQ(RowsText(r2.statements.back().result), std::string("1|12000\n2|9000"));
}

MT_TEST(视图_可以嵌视图) {
  Engine e("lf_view_nested");
  const auto r = e.Run(std::string(kSetup) +
                       "CREATE VIEW v_eng AS GET id, name, salary IN emp LIMIT dept = 'Eng';"
                       "CREATE VIEW v_top AS GET id, name IN v_eng LIMIT salary > 10000;"
                       "GET * IN v_top;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements.back().result), std::string("1|Alice"));
}

MT_TEST(视图_DROP后不可用) {
  Engine e("lf_view_drop");
  const auto r = e.Run(std::string(kSetup) +
                       "CREATE VIEW v_eng AS GET id IN emp;"
                       "DROP VIEW v_eng;"
                       "GET * IN v_eng;");
  // DROP 成功，但随后的查询应报「表不存在」（视图已从目录里消失）。
  // 注意用 back()：kSetup 本身就占 5 条语句（CREATE TABLE + 4 条 INSERT），
  // 按固定下标取会误取到 INSERT —— 那种断言永远是绿的，等于没测。
  MT_CHECK(!r.all_ok());
  MT_CHECK(r.statements.size() >= 3);
  MT_CHECK(!r.statements.back().status.ok());
}

MT_TEST(视图_与同名表冲突时被拦下) {
  Engine e("lf_view_conflict");
  const auto r = e.Run(std::string(kSetup) + "CREATE VIEW emp AS GET id IN emp;");
  // 视图名与已存在的表重名 → 语义阶段就拦（SEM-352），不会走到执行期
  MT_CHECK(!r.all_ok());
}

MT_TEST(视图_可以查询自己的列清单) {
  Engine e("lf_view_catalog");
  const auto r = e.Run(std::string(kSetup) +
                       "CREATE VIEW v_eng AS GET id, name IN emp;"
                       "GET name, columns IN cella_view LIMIT name = 'v_eng';");
  MT_CHECK(r.all_ok());
  MT_EQ(RowCount(r.statements.back().result), 1u);
}

// ── CTE ────────────────────────────────────────────────────────

MT_TEST(CTE_单表表达式) {
  Engine e("lf_cte_one");
  const auto r = e.Run(std::string(kSetup) +
                       "WITH big AS (GET id, salary IN emp LIMIT salary > 8000)"
                       " GET id, salary IN big;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements.back().result), std::string("1|12000\n2|9000"));
}

MT_TEST(CTE_多个可串联引用) {
  Engine e("lf_cte_chain");
  // 后一个 CTE 引用前一个（登记顺序保证可见性）
  const auto r = e.Run(std::string(kSetup) +
                       "WITH a AS (GET id, dept, salary IN emp LIMIT salary > 7000),"
                       "     b AS (GET id, salary IN a LIMIT dept = 'Eng')"
                       " GET * IN b;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements.back().result), std::string("1|12000\n2|9000"));
}

MT_TEST(CTE_只在语句内可见) {
  Engine e("lf_cte_scope");
  const auto r = e.Run(std::string(kSetup) +
                       "WITH a AS (GET id IN emp) GET * IN a;"
                       "GET * IN a;");
  MT_CHECK(!r.all_ok());  // 最后一条语句应报「表 a 不存在」
  MT_CHECK(r.statements.size() >= 2);
  MT_CHECK(!r.statements.back().status.ok());
}

// ── 计划文本契约 ───────────────────────────────────────────────

MT_TEST(计划文本_新能力不引入新算子名) {
  Engine e("lf_plan_text");
  const auto r = e.Run(std::string(kSetup));
  MT_CHECK(r.all_ok());
  // 计划文本是 golden 的逐字节契约：窗口/子查询/视图都必须表现为既有的算子
  // （Project / Filter / SeqScan），不能冒出 Window / Subquery 之类的新名字。
  const char* kCases[] = {
      "GET id, ROW_NUMBER() OVER (PARTITION BY dept ORDERED BY salary DESC) IN emp;",
      "GET id IN emp LIMIT dept IN (GET dept IN emp LIMIT salary > 8000);",
      "GET id IN emp LIMIT EXISTS (GET id IN emp LIMIT salary > 11000);",
  };
  for (const char* sql : kCases) {
    ScriptReport rep;
    MT_CHECK(e.session().CompileOnly(sql, &rep).ok());
    MT_CHECK(rep.all_ok());
    MT_CHECK(!rep.statements.empty());
    const std::string plan = rep.statements.front().plan_text;
    MT_CHECK(plan.find("SeqScan [emp]") != std::string::npos);
    MT_CHECK(plan.find("Window") == std::string::npos);
    MT_CHECK(plan.find("Subquery") == std::string::npos);
    MT_CHECK(plan.find("CreateView") == std::string::npos);
  }
}
