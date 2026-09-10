// test_util.h —— 测试公共设施：临时数据目录、引擎夹具、结果断言辅助。
#pragma once

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "cella/db/common/value_bridge.h"
#include "cella/db/engine/db_engine.h"
#include "cella/db/exec/query_result.h"

#ifdef CELLA_DB_TESTDATA_DIR
#define TEST_ROOT CELLA_DB_TESTDATA_DIR
#else
#define TEST_ROOT "."
#endif

namespace testutil {

using cella::db::DbEngine;
using cella::db::EngineConfig;
using cella::db::QueryResult;
using cella::db::ScriptReport;
using cella::db::StatementOutcome;

// 每次调用都清空并重建的独立数据目录（用例之间互不干扰）
inline std::string FreshDir(const std::string& name) {
  const std::string base = std::string(TEST_ROOT) + "/" + name;
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  return base;
}

// 引擎夹具：Open/Close 自动管理，便于「关掉再打开」的持久化用例
struct Engine {
  DbEngine engine;
  EngineConfig cfg;
  bool opened = false;

  explicit Engine(const std::string& dir_name, size_t pool = 32) {
    cfg.data_dir = FreshDir(dir_name);
    cfg.pool_size = pool;
    cfg.enable_log = false;
    cfg.log_to_console = false;
    opened = engine.Open(cfg).ok();
  }

  ~Engine() { engine.Close(); }

  cella::db::Session& session() { return engine.default_session(); }

  ScriptReport Run(const std::string& sql) {
    ScriptReport r;
    (void)session().Execute(sql, &r);
    return r;
  }

  void Close() { engine.Close(); }

  bool Reopen() {
    engine.Close();
    opened = engine.Open(cfg).ok();
    return opened;
  }
};

// 已打开同一目录的引擎（用于持久化/重启用例）
inline bool ReopenInPlace(Engine* e) { return e->Reopen(); }

// "a|b\nc|d"：把结果集渲染成便于比对的文本
inline std::string RowsText(const QueryResult& r) {
  std::ostringstream os;
  for (size_t i = 0; i < r.rows.size(); ++i) {
    if (i != 0) {
      os << "\n";
    }
    for (size_t j = 0; j < r.rows[i].size(); ++j) {
      if (j != 0) {
        os << "|";
      }
      os << cella::db::RenderValue(r.rows[i][j]);
    }
  }
  return os.str();
}

inline std::string ColsText(const QueryResult& r) {
  std::ostringstream os;
  for (size_t i = 0; i < r.columns.size(); ++i) {
    if (i != 0) {
      os << "|";
    }
    os << r.columns[i].name;
  }
  return os.str();
}

// 执行一段 SQL 并断言全部语句成功；返回最后一条语句的结果
inline bool RunAllOk(cella::db::Session& session, const std::string& sql, ScriptReport* report) {
  (void)session.Execute(sql, report);
  return report->all_ok();
}

inline std::string ReportDigest(const ScriptReport& r) {
  std::ostringstream os;
  for (size_t i = 0; i < r.statements.size(); ++i) {
    const StatementOutcome& s = r.statements[i];
    if (i != 0) {
      os << "\n";
    }
    os << "#" << (i + 1) << " " << s.kind << " ";
    if (!s.status.ok()) {
      os << s.status.ToString();
    } else {
      os << "OK";
    }
  }
  return os.str();
}

// 第一个失败语句的状态文本（无失败返回空）
inline std::string FirstError(const ScriptReport& r) {
  for (const auto& s : r.statements) {
    if (!s.status.ok()) {
      return s.status.ToString();
    }
  }
  return std::string();
}

inline size_t ErrorCount(const ScriptReport& r) {
  size_t n = 0;
  for (const auto& s : r.statements) {
    if (!s.status.ok() || !s.compile_errors.empty()) {
      ++n;
    }
  }
  return n;
}

// 结果集行数
inline size_t RowCount(const QueryResult& r) { return r.rows.size(); }

}  // namespace testutil
