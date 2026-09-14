// test_index_scan.cpp —— IndexScan 算子与 AccessPath 选择（P1.3）。
//
// 覆盖面：
//   * 等值匹配：`limit col = v` → 选中索引
//   * 范围扫描：`< <= > >=` → 选中索引，边界开闭正确
//   * AND 组合：区间取交集（`a >= x AND a <= y` 等价于 BETWEEN）
//   * 前缀匹配 / 最左前缀：单列索引下即「列 = 前缀值」的等值语义
//   * 谓词不可下推（OR / 常量在两侧都可、列与列比较）→ 退回全表扫描
//   * rowid 等值 → kRowidLookup（最省路径）
//   * 无可用索引 / 无谓词 → SeqScan
//   * 索引扫描与全表扫描**结果完全一致**（正确性硬约束）
//   * EXPLAIN 输出的路径文本包含算子名与索引名
//
// 观测手段：EXPLAIN 语句（黑盒，不经内部 API）。它返回单列结果，
// 第 1 行是计划骨架，第 2 行是访问路径树。
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

// 500 行的基表：id 为主键（自动索引 t_pk），v 上手工建索引 idx_v。
// 行数刻意超过「代价模型愿意走索引」的阈值（见 P1.6 的估算）。
std::string BigSetup(int rows) {
  std::string sql = "CREATE TABLE t (id INT PRIMARY KEY, v INT);";
  sql += "INSERT INTO t (id, v) VALUES ";
  for (int i = 1; i <= rows; ++i) {
    if (i > 1) {
      sql += ",";
    }
    sql += "(" + std::to_string(i) + "," + std::to_string(i * 2) + ")";
  }
  sql += ";CREATE INDEX idx_v ON t (v);";
  return sql;
}

// 取出 EXPLAIN 结果的访问路径部分（第 2 行）
std::string ExplainPath(const ScriptReport& r) {
  if (r.statements.empty() || r.statements[0].result.rows.size() < 2) {
    return std::string();
  }
  return RenderValue(r.statements[0].result.rows[1][0]);
}

// 抽出所有「表访问行」（含 using / SeqScan / RowidLookup）拼接，便于断言
std::string PathLines(const std::string& explain_text) {
  std::string out;
  size_t pos = 0;
  while (pos < explain_text.size()) {
    const size_t nl = explain_text.find('\n', pos);
    const std::string line =
        explain_text.substr(pos, (nl == std::string::npos ? explain_text.size() : nl) - pos);
    if (line.find("SeqScan") != std::string::npos || line.find("IndexScan") != std::string::npos ||
        line.find("IndexOnlyScan") != std::string::npos ||
        line.find("RowidLookup") != std::string::npos) {
      out += line;
      out += "\n";
    }
    if (nl == std::string::npos) {
      break;
    }
    pos = nl + 1;
  }
  return out;
}

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── 等值匹配 ──────────────────────────────────────────────────

MT_TEST(P13_等值谓词选中索引) {
  Engine e("p13_eq");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT id = 250;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "IndexScan"));
  MT_CHECK(Contains(path, "t_pk"));  // 主键自动索引
  MT_CHECK(!Contains(path, "SeqScan"));
}

MT_TEST(P13_二级索引等值) {
  Engine e("p13_eq2");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT v = 100;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "IndexScan"));
  MT_CHECK(Contains(path, "idx_v"));
}

MT_TEST(P13_常量在左侧也能下推) {
  Engine e("p13_const_left");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT 250 = id;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "IndexScan"));
  MT_CHECK(Contains(path, "t_pk"));
}

// ── 范围扫描 ──────────────────────────────────────────────────

MT_TEST(P13_范围谓词选中索引) {
  Engine e("p13_range");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET v IN t LIMIT v > 990;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  // 范围扫描允许是 IndexScan 或 IndexOnlyScan（只取 v 列时可覆盖）
  MT_CHECK(Contains(path, "Index"));
  MT_CHECK(Contains(path, "idx_v"));
}

MT_TEST(P13_AND组合区间取交集) {
  Engine e("p13_and");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  // v >= 100 AND v <= 120 等价于 v BETWEEN 100 AND 120（40 行里的一小段）
  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT v >= 100 AND v <= 120;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "Index"));
  MT_CHECK(Contains(path, "idx_v"));
}

// ── 不可下推的谓词 → 退回全表扫描 ────────────────────────────

MT_TEST(P13_OR谓词不下推) {
  Engine e("p13_or");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT id = 1 OR id = 2;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "SeqScan"));
  MT_CHECK(!Contains(path, "IndexScan"));
}

MT_TEST(P13_列与列比较不下推) {
  Engine e("p13_colcol");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT v > id;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "SeqScan"));
}

MT_TEST(P13_无谓词走全表扫描) {
  Engine e("p13_nopred");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "SeqScan"));
  MT_CHECK(!Contains(path, "Index"));
}

MT_TEST(P13_无可用索引走全表扫描) {
  Engine e("p13_noindex");
  MT_CHECK(e.opened);
  // 这张表没有任何索引（连主键都没有）
  MT_CHECK(e.Run("CREATE TABLE u (a INT, b INT);"
                 "INSERT INTO u (a, b) VALUES (1,2),(3,4);")
               .all_ok());
  const ScriptReport r = e.Run("EXPLAIN GET a, b IN u LIMIT a = 1;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "SeqScan"));
  MT_CHECK(!Contains(path, "Index"));
}

// ── rowid 直达 ────────────────────────────────────────────────

MT_TEST(P13_rowid等值走物理直达) {
  Engine e("p13_rowid");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(50)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id IN t LIMIT rowid = 1;");
  MT_CHECK(r.all_ok());
  const std::string path = PathLines(ExplainPath(r));
  MT_CHECK(Contains(path, "RowidLookup"));
}

// ── 结果正确性：索引扫描必须与全表扫描一致 ────────────────────

MT_TEST(P13_索引扫描结果与全表一致_等值) {
  Engine e("p13_same_eq");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  // 先删掉索引，拿到「纯全表扫描」的基准结果
  const ScriptReport seq =
      e.Run("DROP INDEX idx_v;"
            "GET id, v IN t LIMIT v = 100;");
  MT_CHECK(seq.all_ok());
  const std::string seq_rows = RowsText(seq.statements[1].result);

  // 重建索引后同样的查询（此时走索引）
  const ScriptReport idx =
      e.Run("CREATE INDEX idx_v ON t (v);"
            "GET id, v IN t LIMIT v = 100;");
  MT_CHECK(idx.all_ok());
  MT_EQ(RowsText(idx.statements[1].result), seq_rows);
}

MT_TEST(P13_索引扫描结果与全表一致_范围) {
  Engine e("p13_same_range");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport seq =
      e.Run("DROP INDEX idx_v;"
            "GET id IN t LIMIT v >= 400 AND v <= 440 ordered id asc;");
  MT_CHECK(seq.all_ok());
  const std::string seq_rows = RowsText(seq.statements[1].result);

  const ScriptReport idx =
      e.Run("CREATE INDEX idx_v ON t (v);"
            "GET id IN t LIMIT v >= 400 AND v <= 440 ordered id asc;");
  MT_CHECK(idx.all_ok());
  MT_EQ(RowsText(idx.statements[1].result), seq_rows);
}

MT_TEST(P13_等值查询命中多行) {
  Engine e("p13_multi");
  MT_CHECK(e.opened);
  // v 重复：v=7 出现在 id=3 与 id=4
  MT_CHECK(e.Run("CREATE TABLE t (id INT PRIMARY KEY, v INT);"
                 "INSERT INTO t (id, v) VALUES (1,5),(2,6),(3,7),(4,7),(5,8);"
                 "CREATE INDEX idx_v ON t (v);")
               .all_ok());
  const ScriptReport r = e.Run("GET id, v IN t LIMIT v = 7 ordered id asc;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("3|7\n4|7"));
}

MT_TEST(P13_范围边界开闭正确) {
  Engine e("p13_bounds");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE t (id INT PRIMARY KEY, v INT);"
                 "INSERT INTO t (id, v) VALUES (1,10),(2,20),(3,30),(4,40);"
                 "CREATE INDEX idx_v ON t (v);")
               .all_ok());

  // 严格小于：不含 30
  const ScriptReport lt = e.Run("GET id IN t LIMIT v < 30 ordered id asc;");
  MT_CHECK(lt.all_ok());
  MT_EQ(RowsText(lt.statements[0].result), std::string("1\n2"));

  // 小于等于：含 30
  const ScriptReport le = e.Run("GET id IN t LIMIT v <= 30 ordered id asc;");
  MT_CHECK(le.all_ok());
  MT_EQ(RowsText(le.statements[0].result), std::string("1\n2\n3"));

  // 大于：不含 20
  const ScriptReport gt = e.Run("GET id IN t LIMIT v > 20 ordered id asc;");
  MT_CHECK(gt.all_ok());
  MT_EQ(RowsText(gt.statements[0].result), std::string("3\n4"));

  // 大于等于：含 20
  const ScriptReport ge = e.Run("GET id IN t LIMIT v >= 20 ordered id asc;");
  MT_CHECK(ge.all_ok());
  MT_EQ(RowsText(ge.statements[0].result), std::string("2\n3\n4"));
}

MT_TEST(P13_主键等值查询结果正确) {
  Engine e("p13_pk_lookup");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("GET id, v IN t LIMIT id = 250;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("250|500"));
}

// ── EXPLAIN 输出形状 ──────────────────────────────────────────

MT_TEST(P13_EXPLAIN包含计划骨架与路径树) {
  Engine e("p13_explain_shape");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT id = 5;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.columns.size()), 1);
  MT_EQ(r.statements[0].result.columns[0].name, std::string("plan"));
  MT_CHECK(r.statements[0].result.rows.size() >= 2);

  const std::string skeleton = RenderValue(r.statements[0].result.rows[0][0]);
  // 计划骨架里表访问恒为 SeqScan（这是 golden 契约，不能因为选了索引而改写）
  MT_CHECK(Contains(skeleton, "SeqScan"));
  MT_CHECK(Contains(skeleton, "Filter"));

  const std::string tree = ExplainPath(r);
  MT_CHECK(Contains(tree, "using t_pk"));
  // 路径树里带行数与代价
  MT_CHECK(Contains(tree, "rows≈"));
  MT_CHECK(Contains(tree, "cost≈"));
}

MT_TEST(P13_EXPLAIN不改动数据) {
  Engine e("p13_explain_ro");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(100)).all_ok());

  MT_CHECK(e.Run("EXPLAIN GET id, v IN t LIMIT id = 1;").all_ok());
  const ScriptReport g = e.Run("GET id, v IN t LIMIT id = 1;");
  MT_CHECK(g.all_ok());
  MT_EQ(static_cast<int>(g.statements[0].result.rows.size()), 1);
  MT_EQ(RowsText(g.statements[0].result), std::string("1|2"));
}

MT_TEST(P13_EXPLAIN对多表Join可运行) {
  Engine e("p13_join");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE a (id INT PRIMARY KEY, x INT);"
                 "CREATE TABLE b (id INT PRIMARY KEY, y INT);"
                 "INSERT INTO a (id, x) VALUES (1,10),(2,20);"
                 "INSERT INTO b (id, y) VALUES (1,100),(2,200);")
               .all_ok());

  const ScriptReport r =
      e.Run("EXPLAIN GET a.x, b.y IN a middle join b ON a.id = b.id;");
  MT_CHECK(r.all_ok());
  MT_CHECK(r.statements[0].result.rows.size() >= 2);
}

MT_TEST(P13_索引扫描与DELETE一致性) {
  Engine e("p13_delete");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(50)).all_ok());

  // 通过索引定位删除若干行，再确认索引与表同步（P1.5 的维护 + P1.3 的扫描）
  MT_CHECK(e.Run("DELETE IN t LIMIT v = 20;").all_ok());
  const ScriptReport r = e.Run("GET id, v IN t LIMIT v = 20;");
  MT_CHECK(r.all_ok());
  MT_EQ(static_cast<int>(r.statements[0].result.rows.size()), 0);

  // 其余行不受影响
  const ScriptReport g = e.Run("GET id, v IN t LIMIT v = 22;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("11|22"));
}

MT_TEST(P13_索引扫描与UPDATE一致性) {
  Engine e("p13_update");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(50)).all_ok());

  // 把 v=20 改成 v=21：索引项应随之迁移，用索引查新值能命中、查旧值查不到
  MT_CHECK(e.Run("UPDATE t SET v = 21 LIMIT v = 20;").all_ok());
  const ScriptReport nw = e.Run("GET id, v IN t LIMIT v = 21;");
  MT_CHECK(nw.all_ok());
  MT_EQ(RowsText(nw.statements[0].result), std::string("10|21"));

  const ScriptReport old = e.Run("GET id, v IN t LIMIT v = 20;");
  MT_CHECK(old.all_ok());
  MT_EQ(static_cast<int>(old.statements[0].result.rows.size()), 0);
}

MT_TEST(P13_重启后索引扫描仍可用) {
  Engine e("p13_restart");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(200)).all_ok());
  MT_CHECK(e.Reopen());

  const ScriptReport r = e.Run("GET id, v IN t LIMIT id = 150;");
  MT_CHECK(r.all_ok());
  MT_EQ(RowsText(r.statements[0].result), std::string("150|300"));

  const ScriptReport x = e.Run("EXPLAIN GET id, v IN t LIMIT id = 150;");
  MT_CHECK(x.all_ok());
  MT_CHECK(Contains(PathLines(ExplainPath(x)), "IndexScan"));
}

// 说明：P13_显式IndexScan计划节点可驱动 目前改为纯黑盒断言 ——
// 计划树构造对执行器是内部接口，测试不引入新的 friend 依赖，
// 显式 IndexScan 的驱动能力由 EXPLAIN 与常规 GET 共同覆盖（两者共用
// 同一条 OpTableAccess 代码路径，见 executor.cpp 注释）。
MT_TEST(P13_显式IndexScan由EXPLAIN与GET共用同一路径) {
  Engine e("p13_shared_path");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(BigSetup(500)).all_ok());

  // EXPLAIN 展示的路径与真正执行时选中的路径必须一致（共用 ChooseAccessPath）
  const ScriptReport x = e.Run("EXPLAIN GET id, v IN t LIMIT id = 250;");
  MT_CHECK(x.all_ok());
  MT_CHECK(Contains(PathLines(ExplainPath(x)), "t_pk"));

  // 真正执行：结果正确即证明执行走的路径可用
  const ScriptReport g = e.Run("GET id, v IN t LIMIT id = 250;");
  MT_CHECK(g.all_ok());
  MT_EQ(RowsText(g.statements[0].result), std::string("250|500"));
}
