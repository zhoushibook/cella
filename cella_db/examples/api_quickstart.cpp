// api_quickstart.cpp —— 30 秒 API 速览：用 C++ 直接驱动整合后的数据库系统。
//
// 覆盖：建库 → 会话执行 SQL → 读结果集 → 显式事务 → 诊断接口。
// 运行：build/cella_db/api_quickstart.exe   （在 build/cella_db 目录下执行）
#include <filesystem>
#include <iostream>
#include <string>

#include "cella/db/common/value_bridge.h"
#include "cella/db/engine/db_engine.h"

using cella::db::DbEngine;
using cella::db::DbStatus;
using cella::db::EngineConfig;
using cella::db::QueryResult;
using cella::db::ScriptReport;

int main() {
  // ① 配置并打开引擎（数据目录、页大小、缓冲池规模、替换策略）
  EngineConfig cfg;
  cfg.data_dir = "./quickstart_data";
  cfg.db_file = "quickstart.db";
  cfg.page_size = 4096;
  cfg.pool_size = 32;
  cfg.replacer = "LRU";
  cfg.enable_log = true;   // 日志写到 quickstart_data/cella-db.log
  cfg.enable_journal = true;
  // 从空库开始，便于反复运行
  {
    std::error_code ec;
    std::filesystem::remove_all(cfg.data_dir, ec);
  }

  DbEngine engine;
  const DbStatus st = engine.Open(cfg);
  if (!st.ok()) {
    std::cerr << "打开失败: " << st.ToString() << "\n";
    return 1;
  }
  std::cout << "引擎已打开，数据目录 " << cfg.data_dir << "\n\n";

  // ② 通过会话执行 SQL（多条语句一次提交，逐条汇报）
  ScriptReport report;
  (void)engine.default_session().Execute(
      "CREATE TABLE student(id INT NOT NULL, name VARCHAR(16) NOT NULL, score DOUBLE);"
      "INSERT INTO student VALUES (1,'Alice',88.5),(2,'Bob',76.0),(3,'Carol',94.5);",
      &report);
  for (const auto& s : report.statements) {
    std::cout << "[" << s.kind << "] " << (s.status.ok() ? "OK" : s.status.ToString()) << "\n";
  }

  // ③ 查询：结果集就是内存里的二维表
  (void)engine.default_session().Execute("get id, name, score in student ordered score desc;",
                                         &report);
  const QueryResult& q = report.statements[0].result;
  for (const auto& col : q.columns) {
    std::cout << col.name << "\t";
  }
  std::cout << "\n";
  for (const auto& row : q.rows) {
    for (const auto& v : row) {
      std::cout << cella::db::RenderValue(v) << "\t";
    }
    std::cout << "\n";
  }

  // ④ 显式事务：提交生效、回滚撤销
  (void)engine.default_session().Execute("BEGIN;", &report);
  (void)engine.default_session().Execute("UPDATE student SET score = 0 limit id = 2;", &report);
  (void)engine.default_session().Execute("ROLLBACK;", &report);
  (void)engine.default_session().Execute("get score in student limit id = 2;", &report);
  std::cout << "\n回滚后 id=2 的 score = "
            << cella::db::RenderValue(report.statements[0].result.rows[0][0]) << "（应为 76）\n";

  // ⑤ 诊断接口：缓冲池 / 锁 / 事务 / 目录
  std::cout << "\n--- 目录 ---\n" << engine.catalog().Describe();
  std::cout << "\n--- 缓冲池 ---\n" << engine.StatsText();

  engine.Close();
  std::cout << "\n已关闭。\n";
  return 0;
}
