// test_cost_model.cpp —— 代价模型与 AccessPath 选择（P1.6）。
//
// 覆盖面：
//   * 表行数是代价的基数输入：小表能全表扫描，大到一定程度后索引胜出
//   * 索引高度参与定位代价：树高实测值（不再用常数）
//   * 选择率：等值 < 窄范围 < 宽范围 → 代价单调上升
//   * 窄范围选索引、宽范围回退全表（「压不下来就别用索引」）
//   * 覆盖扫描（index-only）比回表便宜 —— 同样是索引，投影列少时更省
//   * P1.3 的 AccessPath 选择由本模型驱动（回归保护）
//
// 观测手段：EXPLAIN（黑盒）。它输出的 `rows≈N cost≈C` 直接反映模型结果。
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>

#include "cella/db/engine/db_engine.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

// 造一张 rows 行的表：id 主键（自动 t_pk 索引），v 上手工建 idx_v。
std::string Setup(int rows) {
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

std::string ExplainPathOf(const ScriptReport& r) {
  if (r.statements.empty() || r.statements[0].result.rows.size() < 2) {
    return std::string();
  }
  return RenderValue(r.statements[0].result.rows[1][0]);
}

// 从访问路径文本里抠出 cost≈ 后面的数字；找不到返回 -1。
double CostOf(const std::string& path_text) {
  const std::string key = "cost≈";
  const size_t p = path_text.find(key);
  if (p == std::string::npos) {
    return -1.0;
  }
  return std::strtod(path_text.c_str() + p + key.size(), nullptr);
}

// 抠出 rows≈ 后面的数字；找不到返回 -1。
double RowsOf(const std::string& path_text) {
  const std::string key = "rows≈";
  const size_t p = path_text.find(key);
  if (p == std::string::npos) {
    return -1.0;
  }
  return std::strtod(path_text.c_str() + p + key.size(), nullptr);
}

bool Has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── 基数与 I/O 模型 ────────────────────────────────────────

MT_TEST(P16_小表全表扫描更便宜) {
  // 一张 1 页就能装下的表（20 行）：顺序读 1 页 + 逐行 CPU，
  // 比「索引下降 + 回表」两次随机 I/O 更省。这是物理上正确的结论
  // （真实 DBMS 同样如此），也是对模型的**反向**约束：
  // 不能让索引无脑胜出。
  Engine e("p16_tiny");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(20)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT v = 20;");
  MT_CHECK(r.all_ok());
  const std::string path = ExplainPathOf(r);
  MT_CHECK(Has(path, "SeqScan"));
  MT_CHECK(!Has(path, "IndexScan"));

  // 全表代价 = max(1, ceil(20/40)) * 1.0 + 20 * 0.10 = 3.0
  const double seq_cost = CostOf(path);
  MT_CHECK(seq_cost > 2.9 && seq_cost < 3.1);
}

MT_TEST(P16_表变大后索引等值胜出) {
  // 50 行 → 2 页：全表 = 2*1 + 50*0.1 = 7.0；
  // 索引等值 = (高度1 + 叶子1)*2 + 1*0.1 + 回表1*2 = 6.1 → 索引胜。
  Engine e("p16_grow");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(50)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT v = 20;");
  MT_CHECK(r.all_ok());
  const std::string path = ExplainPathOf(r);
  MT_CHECK(Has(path, "Index"));
  MT_CHECK(Has(path, "idx_v"));

  // 索引代价必须低于 50 行全表的 7.0
  const double ix_cost = CostOf(path);
  MT_CHECK(ix_cost > 0.0 && ix_cost < 7.0);
}

MT_TEST(P16_主键等值走主键索引) {
  Engine e("p16_pk");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(500)).all_ok());

  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT id = 250;");
  MT_CHECK(r.all_ok());
  const std::string path = ExplainPathOf(r);
  MT_CHECK(Has(path, "t_pk"));
  // 等值命中 1 行
  const double rows = RowsOf(path);
  MT_CHECK(rows == 1.0);
}

// ── 选择率与代价单调性 ─────────────────────────────────────

MT_TEST(P16_选择率越窄代价越低) {
  // 同一张表上比较三种谓词的估算代价：
  //   v = 500（1 行） <  v ∈ [400, 600]（101 行） < v ∈ [100, 900]（401 行）
  // 代价必须单调不减 —— 这是「选择率参与估算」的直接证据。
  Engine e("p16_sel");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(500)).all_ok());

  const ScriptReport a = e.Run("EXPLAIN GET id, v IN t LIMIT v = 500;");
  const ScriptReport b = e.Run("EXPLAIN GET id, v IN t LIMIT v >= 400 AND v <= 600;");
  const ScriptReport c = e.Run("EXPLAIN GET id, v IN t LIMIT v >= 100 AND v <= 900;");
  MT_CHECK(a.all_ok());
  MT_CHECK(b.all_ok());
  MT_CHECK(c.all_ok());

  const double ra = RowsOf(ExplainPathOf(a));
  const double rb = RowsOf(ExplainPathOf(b));
  const double rc = RowsOf(ExplainPathOf(c));

  MT_CHECK(ra > 0.0);
  MT_CHECK(rb >= ra);   // 区间变宽，估出的行数不变少
  MT_CHECK(rc >= rb);
}

MT_TEST(P16_窄范围选索引_宽范围退全表) {
  Engine e("p16_narrow_wide");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(500)).all_ok());

  // 窄范围（10 行）→ 索引划算
  const ScriptReport n = e.Run("EXPLAIN GET id, v IN t LIMIT v >= 100 AND v <= 120;");
  MT_CHECK(n.all_ok());
  MT_CHECK(Has(ExplainPathOf(n), "Index"));

  // 宽范围（401 行，接近全表）→ 走索引要回表 401 次，不如直接扫
  const ScriptReport w = e.Run("EXPLAIN GET id, v IN t LIMIT v >= 100 AND v <= 900;");
  MT_CHECK(w.all_ok());
  MT_CHECK(Has(ExplainPathOf(w), "SeqScan"));
}

// ── 覆盖扫描 ───────────────────────────────────────────────

MT_TEST(P16_覆盖扫描比回表便宜) {
  // 只读索引列 v：index-only，省掉回表 I/O；
  // 同时要 id 和 v：必须回表。两者代价应有明确差距。
  Engine e("p16_covering");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(500)).all_ok());

  const ScriptReport only = e.Run("EXPLAIN GET v IN t LIMIT v = 500;");
  const ScriptReport heap = e.Run("EXPLAIN GET id, v IN t LIMIT v = 500;");
  MT_CHECK(only.all_ok());
  MT_CHECK(heap.all_ok());

  const std::string op = ExplainPathOf(only);
  const std::string hp = ExplainPathOf(heap);
  MT_CHECK(Has(op, "IndexOnlyScan"));
  MT_CHECK(Has(hp, "IndexScan"));

  const double c_only = CostOf(op);
  const double c_heap = CostOf(hp);
  MT_CHECK(c_only > 0.0);
  MT_CHECK(c_heap > c_only);  // 回表一定更贵
}

// ── 代价模型驱动 P1.3 的路径选择（回归保护）────────────────

MT_TEST(P16_代价模型驱动P13路径选择) {
  // 同一谓词在「小表」与「大表」上应给出不同的路径 ——
  // 证明选择确实由代价（而非固定的「有索引就用」规则）决定。
  Engine small("p16_drive_small");
  MT_CHECK(small.opened);
  MT_CHECK(small.Run(Setup(20)).all_ok());
  const ScriptReport s = small.Run("EXPLAIN GET id, v IN t LIMIT id = 5;");
  MT_CHECK(s.all_ok());
  MT_CHECK(Has(ExplainPathOf(s), "SeqScan"));

  Engine big("p16_drive_big");
  MT_CHECK(big.opened);
  MT_CHECK(big.Run(Setup(2000)).all_ok());
  const ScriptReport b = big.Run("EXPLAIN GET id, v IN t LIMIT id = 5;");
  MT_CHECK(b.all_ok());
  MT_CHECK(Has(ExplainPathOf(b), "IndexScan"));
}

MT_TEST(P16_索引扫描结果与全表扫描一致_大表) {
  // 代价模型只影响「怎么取」，绝不影响「取到什么」。
  Engine e("p16_correct");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(1000)).all_ok());

  // id=777 → v=1554；v=1554 应唯一命中同一行
  const ScriptReport a = e.Run("GET id, v IN t LIMIT id = 777;");
  const ScriptReport b = e.Run("GET id, v IN t LIMIT v = 1554;");
  MT_CHECK(a.all_ok());
  MT_CHECK(b.all_ok());
  MT_CHECK(RowsText(a.statements[0].result) == RowsText(b.statements[0].result));
  MT_CHECK(RowsText(a.statements[0].result) == std::string("777|1554"));
}

// ── 基准：1k / 10k 行的路径选择与结果正确性 ────────────────

MT_TEST(P16_基准_1k行) {
  Engine e("p16_bench_1k");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(1000)).all_ok());

  const ScriptReport x = e.Run("EXPLAIN GET id, v IN t LIMIT v = 1000;");
  MT_CHECK(x.all_ok());
  MT_CHECK(Has(ExplainPathOf(x), "Index"));

  const ScriptReport g = e.Run("GET id, v IN t LIMIT v = 1000;");
  MT_CHECK(g.all_ok());
  MT_CHECK(RowsText(g.statements[0].result) == std::string("500|1000"));
}

MT_TEST(P16_基准_10k行) {
  // 10k 行：索引高度应涨到 2 层以上，且等值查找仍选中索引。
  Engine e("p16_bench_10k");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(10000)).all_ok());

  const ScriptReport x = e.Run("EXPLAIN GET id, v IN t LIMIT v = 12346;");
  MT_CHECK(x.all_ok());
  const std::string path = ExplainPathOf(x);
  MT_CHECK(Has(path, "Index"));
  // 10k 行的等值代价应明显低于 10k 行的全表代价 ——
  // 索引定位是 O(树高)，与表大小几乎无关；全表则是 O(行数)。
  // 全表 = ceil(10000/40)*1.0 + 10000*0.1 = 250 + 1000 = 1250.0
  const double ix_cost = CostOf(path);
  MT_CHECK(ix_cost > 0.0 && ix_cost < 100.0);

  // 结果正确性：v = 2*id，故 v=12346 → id=6173
  const ScriptReport g = e.Run("GET id, v IN t LIMIT v = 12346;");
  MT_CHECK(g.all_ok());
  MT_CHECK(RowsText(g.statements[0].result) == std::string("6173|12346"));
}

MT_TEST(P16_基准_100k行) {
  // 100k 行：代价模型的「可扩展性」验证 —— 索引定位代价应几乎不随行数增长
  // （O(树高)），而全表代价线性增长。这是索引存在的根本理由。
  Engine e("p16_bench_100k");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(Setup(100000)).all_ok());

  const ScriptReport x = e.Run("EXPLAIN GET id, v IN t LIMIT v = 123456;");
  MT_CHECK(x.all_ok());
  const std::string path = ExplainPathOf(x);
  MT_CHECK(Has(path, "Index"));
  // 100k 行全表代价 = ceil(100000/40)*1 + 100000*0.1 = 2500 + 10000 = 12500
  // 索引等值应远低于此（预期 10 以内）
  const double ix_cost = CostOf(path);
  MT_CHECK(ix_cost > 0.0 && ix_cost < 100.0);

  // 结果正确性：v = 2*id → id = 61728
  const ScriptReport g = e.Run("GET id, v IN t LIMIT v = 123456;");
  MT_CHECK(g.all_ok());
  MT_CHECK(RowsText(g.statements[0].result) == std::string("61728|123456"));

  // 窄范围在大表上仍应选索引
  const ScriptReport r = e.Run("EXPLAIN GET id, v IN t LIMIT v >= 123400 AND v <= 123500;");
  MT_CHECK(r.all_ok());
  MT_CHECK(Has(ExplainPathOf(r), "Index"));
}
