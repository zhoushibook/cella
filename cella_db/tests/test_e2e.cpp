// test_e2e.cpp —— 端到端回归：拿编译器原有的测试 SQL（ok_*/err_*）直接跑整合系统。
//
// 这一组用例的价值在于「双向验证」：
//   * 对编译器：整合改造（抽出静态库 + 计划节点附加执行期字段）没有改变任何
//     文本输出 —— 逐条语句重建的计划文本与 tests/expected/*.txt golden 完全一致。
//   * 对整合层：编译器认定的合法 SQL 能真正跑通完整链路（编译 → 调度 → 存储），
//     编译器认定的非法 SQL 在系统里仍然报出同一个错误码。
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;

namespace fs = std::filesystem;

namespace {

#ifdef CELLA_DB_SQL_DIR
const char* const kSqlDir = CELLA_DB_SQL_DIR;
#else
const char* const kSqlDir = ".";
#endif

std::string ReadText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return std::string();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// 归一化：CRLF → LF，去掉行尾空白与首尾空行
std::string Normalize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r') {
      continue;
    }
    out += s[i];
  }
  // 逐行去尾部空白
  std::string cleaned;
  std::istringstream in(out);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (!first) {
      cleaned += "\n";
    }
    cleaned += line;
    first = false;
  }
  while (!cleaned.empty() && cleaned.back() == '\n') {
    cleaned.pop_back();
  }
  while (!cleaned.empty() && cleaned.front() == '\n') {
    cleaned.erase(cleaned.begin());
  }
  return cleaned;
}

// 由逐条语句的「优化前计划」重建整份计划文本（与 cella_printPlan 的多语句排版一致）
std::string RebuildPlanText(const ScriptReport& r) {
  std::string plan;
  bool first = true;
  for (const auto& s : r.statements) {
    if (s.original_plan_text.empty()) {
      continue;
    }
    if (!first) {
      plan += "\n";
    }
    first = false;
    plan += s.original_plan_text;
  }
  return plan;
}

std::vector<fs::path> ListSql(const std::string& prefix) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!fs::exists(kSqlDir, ec)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(kSqlDir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".sql") {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// 从 err_*.sql 首行注释提取期望错误码：-- expect: SEM-307
std::string ExpectedCode(const std::string& sql_text) {
  std::istringstream in(sql_text);
  std::string line;
  std::getline(in, line);
  const std::string key = "expect";
  const size_t at = line.find(key);
  if (at == std::string::npos) {
    return std::string();
  }
  size_t i = at + key.size();
  while (i < line.size() && (line[i] == ':' || line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  std::string code;
  while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '-')) {
    code += line[i];
    ++i;
  }
  return code;
}

}  // namespace

MT_TEST(端到端_编译器正向用例全链路可执行且计划与golden一致) {
  const auto files = ListSql("ok_");
  MT_CHECK(!files.empty());
  size_t goldens_checked = 0;
  for (const auto& f : files) {
    const std::string name = f.stem().string();
    const std::string sql = ReadText(f);
    Engine e("e2e_ok_" + name);
    MT_CHECK(e.opened);

    ScriptReport r;
    (void)e.session().Execute(sql, &r);
    if (!r.all_ok()) {
      std::ostringstream os;
      os << "      " << name << " 失败: " << testutil::ReportDigest(r);
      mt::Say(os.str());
    }
    MT_CHECK(r.all_ok());

    // 与 golden 计划对照
    const fs::path golden = fs::path(kSqlDir).parent_path() / "expected" / (name + "_plan.txt");
    if (!fs::exists(golden)) {
      continue;
    }
    ++goldens_checked;
    const std::string want = Normalize(ReadText(golden));
    const std::string got = Normalize(RebuildPlanText(r));
    if (want != got) {
      std::ostringstream os;
      os << "      golden 不一致: " << name << "\n--- 期望 ---\n" << want << "\n--- 实际 ---\n"
         << got;
      mt::Say(os.str());
    }
    MT_CHECK(want == got);
  }
  MT_CHECK(goldens_checked >= 10);
}

MT_TEST(端到端_编译器负向用例仍报同一错误码) {
  const auto files = ListSql("err_");
  MT_CHECK(!files.empty());
  size_t checked = 0;
  for (const auto& f : files) {
    const std::string sql = ReadText(f);
    const std::string expect = ExpectedCode(sql);
    Engine e("e2e_err_" + f.stem().string());
    ScriptReport r;
    (void)e.session().Execute(sql, &r);

    // 全部诊断文本
    std::string diag;
    for (const auto& s : r.statements) {
      for (const auto& ce : s.compile_errors) {
        diag += ce.code;
        diag += " ";
      }
      if (!s.compile_errors.empty()) {
        diag += "\n";
      }
    }
    if (!expect.empty() && diag.find(expect) == std::string::npos) {
      std::ostringstream os;
      os << "      " << f.filename().string() << " 未报出 " << expect << "；实际诊断: " << diag;
      mt::Say(os.str());
    }
    MT_CHECK(expect.empty() || diag.find(expect) != std::string::npos);
    // 负向用例必须至少有一条语句失败
    MT_CHECK(testutil::ErrorCount(r) >= 1);
    ++checked;
  }
  MT_CHECK(checked >= 16);
}

MT_TEST(端到端_演示脚本_查询无失败) {
#ifdef CELLA_DB_SCRIPT_DIR
  const fs::path f = fs::path(CELLA_DB_SCRIPT_DIR) / "demo_query.sql";
  Engine e("e2e_demo_query");
  const std::string sql = ReadText(f);
  MT_CHECK(!sql.empty());
  const ScriptReport r = e.Run(sql);
  MT_CHECK(r.all_ok());
  MT_EQ(r.statements.size(), 21u);
  MT_EQ(r.statements[0].kind, std::string("CREATE TABLE"));
#endif
}

MT_TEST(端到端_演示脚本_基础用例含预期错误) {
#ifdef CELLA_DB_SCRIPT_DIR
  const fs::path f = fs::path(CELLA_DB_SCRIPT_DIR) / "demo_basic.sql";
  Engine e("e2e_demo_basic");
  const std::string sql = ReadText(f);
  MT_CHECK(!sql.empty());
  const ScriptReport r = e.Run(sql);
  MT_EQ(r.statements.size(), 15u);
  MT_EQ(testutil::ErrorCount(r), 3u);  // 2 条编译期约束 + 1 条运行期类型错误
  // 运行期失败那条处于自动提交模式 → 整个自动事务被回滚（数据同样不留半截）
  MT_CHECK(r.statements[12].auto_committed);
  MT_CHECK(r.statements[12].status.code() == DbCode::kTypeMismatch);
  // 紧跟其后的查询看不到失败语句插入的第一行（id=10）
  MT_EQ(testutil::RowsText(r.statements[13].result), std::string("1|Alice\n3|Carol\n4|Dave\n5|Eve"));
#endif
}

MT_TEST(端到端_演示脚本_事务语义) {
#ifdef CELLA_DB_SCRIPT_DIR
  const fs::path f = fs::path(CELLA_DB_SCRIPT_DIR) / "demo_txn.sql";
  Engine e("e2e_demo_txn");
  const std::string sql = ReadText(f);
  MT_CHECK(!sql.empty());
  const ScriptReport r = e.Run(sql);
  MT_EQ(r.statements.size(), 36u);
  MT_EQ(testutil::ErrorCount(r), 1u);  // 仅语句级原子性那条故意失败
  MT_CHECK(r.statements[23].rolled_back_here);
  MT_CHECK(!e.session().in_transaction());
#endif
}

MT_TEST(端到端_重启后数据与目录均一致) {
  const std::string dir_sql =
      "CREATE TABLE t(id INT NOT NULL, s VARCHAR(16), d DOUBLE);"
      "INSERT INTO t VALUES (1,'a',1.5),(2,'b',2.5),(3,'c',3.5);";
  Engine e("e2e_restart");
  MT_CHECK(e.Run(dir_sql).all_ok());
  MT_CHECK(e.Run("UPDATE t SET s = 'z' limit id = 2;").all_ok());
  MT_CHECK(e.Run("DELETE in t limit id = 3;").all_ok());
  e.Close();
  MT_CHECK(e.Reopen());
  // 目录（元数据）
  const CatalogTable* meta = e.engine.catalog().FindTable("t");
  MT_CHECK(meta != nullptr);
  MT_EQ(meta->columns.size(), 3u);
  MT_CHECK(meta->columns[0].not_null);
  MT_CHECK(meta->columns[2].type == cella::CELLA_DataType::DOUBLE);
  // 数据
  MT_EQ(testutil::RowsText(e.Run("get id, s, d in t ordered id asc;").statements[0].result),
        std::string("1|a|1.5\n2|z|2.5"));
}

MT_TEST(端到端_缓冲池统计可观测) {
  Engine e("e2e_stats", /*pool=*/4);
  MT_CHECK(e.Run("CREATE TABLE big(id INT, s VARCHAR(128));").all_ok());
  // 5000 行 × 约 140 字节 → 远超 4 帧缓冲池，必然触发淘汰
  std::string insert = "INSERT INTO big VALUES ";
  for (int i = 0; i < 5000; ++i) {
    if (i != 0) {
      insert += ",";
    }
    insert += "(" + std::to_string(i) + ",'padding-row-" + std::to_string(i) +
              "-0123456789012345678901234567890123456789')";
  }
  insert += ";";
  MT_CHECK(e.Run(insert).all_ok());
  const ScriptReport r = e.Run("get id in big ordered id desc among 1;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::RowsText(r.statements[0].result), std::string("4999"));

  const std::string stats = e.engine.StatsText();
  MT_CHECK(!stats.empty());
  MT_CHECK(stats.find("缓冲池") != std::string::npos);
  MT_CHECK(!e.engine.storage()->recent_evictions().empty());
}
