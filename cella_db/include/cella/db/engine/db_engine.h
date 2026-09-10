// db_engine.h —— 数据库门面（DbEngine）与会话（Session）。
//
// 分层与职责（自上而下）：
//
//   ┌───────────────────────────────────────────────────────────┐
//   │ CLI (main.cpp)  交互 REPL / 脚本 / 元命令                  │
//   ├───────────────────────────────────────────────────────────┤
//   │ Session         会话：事务上下文、逐语句调度、错误/回滚策略  │
//   ├───────────────────────────────────────────────────────────┤
//   │ DbEngine        门面：持有存储/目录/锁/事务管理器，生命周期  │
//   ├───────────────────────────────────────────────────────────┤
//   │ Executor        计划驱动执行；算子 → 存储调用               │
//   ├───────────────────────────────────────────────────────────┤
//   │ IStorage        页式存储 + 缓冲池（内存管理）               │
//   └───────────────────────────────────────────────────────────┘
//
// 编译器（cella_sql_core）横向插入：Session 调用它完成 文本 → Token → AST →
// 语义 → 计划 → 优化，然后交给 Executor 执行。编译器不感知存储，存储不感知 SQL。
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cella/cella_ast.h"
#include "cella/cella_catalog.h"
#include "cella/cella_common.h"
#include "cella/cella_planner.h"
#include "cella/db/catalog/catalog_manager.h"
#include "cella/db/common/db_logger.h"
#include "cella/db/common/db_status.h"
#include "cella/db/common/time_util.h"
#include "cella/db/engine/sql_text.h"
#include "cella/db/exec/executor.h"
#include "cella/db/exec/query_result.h"
#include "cella/db/txn/lock_manager.h"
#include "cella/db/txn/transaction.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/config.h"

namespace cella::db {

// ── 引擎配置 ────────────────────────────────────────────────
struct EngineConfig {
  std::string data_dir = "./cella_data";
  std::string db_file = "cella.db";
  // 每次提交都做一次存盘点（把数据文件真正落盘）。默认关：会重置缓冲池统计计数器，
  // 批量导入时每次提交都刷盘也偏慢。交互式会话可用 CLI 的 \checkpoint 手动触发。
  bool checkpoint_on_commit = false;
  uint32_t page_size = 4096;             // 页大小（存储层配置）
  size_t pool_size = 64;                 // 缓冲池帧数（内存管理规模）
  std::string replacer = "LRU";          // LRU / FIFO / CLOCK
  bool enable_log = true;
  std::string log_path;                  // 空 → <data_dir>/cella-db.log
  bool log_to_console = false;           // 同时打到控制台
  storage::LogLevel log_level = storage::LogLevel::kInfo;
  std::chrono::milliseconds lock_timeout{5000};
  bool enable_journal = true;            // 写 <data_dir>/journal.log
};

// ── 单条语句的执行记录 ──────────────────────────────────────
struct StatementOutcome {
  std::string sql;                        // 原始语句文本
  std::string kind;                       // 语句算子名（CREATE TABLE / GET / ...）
  int line = 0;
  int col = 0;
  bool compiled = false;                  // 是否通过编译阶段
  bool executed = false;                  // 是否真正执行
  bool auto_committed = false;            // 是否由自动提交收尾
  bool implicit_commit = false;           // DDL 是否隐式提交了前置事务
  bool rolled_back_here = false;          // 是否因本语句失败触发了语句级回滚
  std::string notice;                     // 面向用户的提示（如隐式提交说明）
  txn_id_t txn_id = kInvalidTxnId;
  QueryResult result;
  std::vector<cella::CELLA_Error> compile_errors;
  std::string plan_text;                  // 实际执行所用的（优化后）计划
  std::string original_plan_text;         // 优化前计划（诊断对比用）
  double elapsed_ms = 0.0;
  size_t operator_calls = 0;
  DbStatus status;                        // 语句级状态

  bool ok() const { return status.ok(); }
};

// ── 一次调用的整体汇报 ──────────────────────────────────────
struct ScriptReport {
  std::vector<StatementOutcome> statements;
  size_t ok_count = 0;
  size_t error_count = 0;
  size_t compile_error_count = 0;

  bool all_ok() const { return error_count == 0 && compile_error_count == 0; }
  void Tally();
  std::string ToText(bool verbose) const;
};

class Session;

// ── 数据库引擎（进程内单实例；持有全部共享组件）────────────
class DbEngine {
 public:
  DbEngine();
  ~DbEngine();
  DbEngine(const DbEngine&) = delete;
  DbEngine& operator=(const DbEngine&) = delete;

  DbStatus Open(const EngineConfig& config);
  void Close();
  bool opened() const { return opened_; }

  // 存盘点：把数据文件（含目录页与脏数据页）真正落盘。
  // 存储层只在 Close() 时 FlushAllPages，且 IStorage 没有「flush all」接口，
  // 因此这里用公开的 Close + Open 组合实现一次存盘；调用期间会独占存储互斥量，
  // 不会与其它会话的存储访问交叉。副作用：缓冲池统计计数器会被重置（已在
  // StatsText 里累计历史值，观测不丢）。
  DbStatus Checkpoint();

  // 启动时的自愈记录（目录与数据文件不一致并已自动修复时非空）
  const std::vector<std::string>& recoveries() const { return recoveries_; }
  bool has_recoveries() const { return !recoveries_.empty(); }
  std::string RecoveryReport() const;

  const EngineConfig& config() const { return config_; }
  CatalogManager& catalog() { return catalog_; }
  LockManager& locks() { return *locks_; }
  TxnManager& txn_manager() { return *txn_manager_; }
  storage::IStorage* storage() { return storage_.get(); }

  // 诊断文本
  std::string StatsText();
  // 目录与数据文件不一致时的自愈（Open 内部调用）
  DbStatus ReconcileCatalogWithStorage();
  std::string LockText() const;
  std::string WaitForGraphText() const;
  std::string TxnText() const;

  // 默认会话（CLI 直接用它）
  Session& default_session() { return *default_session_; }

 private:
  friend class Session;
  EngineConfig config_;
  bool opened_ = false;
  std::unique_ptr<storage::IStorage> storage_;
  std::unique_ptr<storage::ILogger> logger_;       // 文件后端
  std::unique_ptr<storage::ILogger> console_logger_;  // 可选控制台后端
  std::unique_ptr<storage::ILogger> tee_logger_;   // logger_ (+console_logger_) 的封装
  CatalogManager catalog_;
  std::unique_ptr<LockManager> locks_;
  std::unique_ptr<TxnManager> txn_manager_;
  std::unique_ptr<Executor> executor_;
  std::unique_ptr<Session> default_session_;
  std::recursive_mutex storage_mutex_;  // 存储层访问串行化（存储层非线程安全）

  // 存盘点需要重开存储引擎，故保留一份存储层配置
  storage::StorageConfig storage_config_;
  // 存盘点会重建缓冲池（统计计数器清零），故把历史值累计在此，保证观测连续
  storage::BufferStats stats_before_checkpoints_;
  uint32_t checkpoint_count_ = 0;
  // 启动时自愈记录
  std::vector<std::string> recoveries_;
};

// ── 会话：一条 SQL 执行链路 ─────────────────────────────────
class Session {
 public:
  Session(DbEngine* engine, std::string name);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  const std::string& name() const { return name_; }

  // 执行一段 SQL（可含多条语句）。逐条编译、逐条执行、逐条汇报。
  DbStatus Execute(const std::string& sql, ScriptReport* report);
  // 执行单条语句
  DbStatus ExecuteOne(const std::string& stmt_text, int line, int col, StatementOutcome* out);
  // 只编译不执行（--show-plan / \plan）
  DbStatus CompileOnly(const std::string& sql, ScriptReport* report);

  // 事务控制
  DbStatus Begin(std::string* note);
  DbStatus Commit(std::string* note);
  DbStatus Rollback(std::string* note);
  bool in_transaction() const { return txn_ != kInvalidTxnId; }
  txn_id_t current_txn() const { return txn_; }

  // 会话状态一行摘要
  std::string StatusLine() const;

 private:
  // 让编译器视角的目录与权威目录（CatalogManager）保持一致
  void EnsureCatalogInSync();
  txn_id_t BeginInternal();

  DbEngine* engine_;
  std::string name_;
  txn_id_t txn_ = kInvalidTxnId;
  std::shared_ptr<Transaction> txn_handle_;
  cella::CELLA_Catalog compiler_catalog_;  // 编译器视角的目录（每条语句后与 Catalog 对齐）
};

}  // namespace cella::db
