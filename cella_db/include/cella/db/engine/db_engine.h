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
#include "cella/db/auth/auth_parser.h"
#include "cella/db/auth/auth_store.h"
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
  // 数据文件名。默认 main.db = 默认库 main；`--db school` 等价于 db_file=school.db。
  std::string db_file = "main.db";
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
  // 访问控制（默认关：关掉时跳过全部权限检查、免登录，行为与引入本特性前一致）。
  // 用户/权限语句本身始终可用（会按需创建身份库），「强制」由本开关控制。
  bool enable_auth = false;
  std::string auth_file;                 // 身份库文件名；空 → cella_auth.db
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

  const EngineConfig& config() const { return config_; }
  CatalogManager& catalog() { return catalog_; }
  LockManager& locks() { return *locks_; }
  TxnManager& txn_manager() { return *txn_manager_; }
  storage::IStorage* storage() { return storage_.get(); }

  // ── 访问控制（身份库 = <data_dir>/cella_auth.db，独立于当前库）──
  AuthStore& auth() { return auth_; }
  bool auth_enabled() const { return config_.enable_auth; }
  // 打开身份库（幂等；用户/权限语句与认证都会按需调用）。
  // enable_auth 且库中尚无用户时，自动引导创建管理员 root（空口令）。
  DbStatus EnsureAuthOpen();
  // 认证：成功返回 Ok 并给出管理员标志；失败 → DB-801（不区分用户名/口令错）。
  DbStatus Authenticate(const std::string& user, const std::string& password, bool* out_is_admin);
  // 身份库即改即落盘：Close + Open 强制刷全（Close = FlushAllPages）。
  // 由 AuthStore 的写后钩子调用；不这样做的后果是「改口令后进程被强杀 → 改动丢失」。
  DbStatus FlushAuth();
  // 首次开启认证时自动建 root 的说明（空 = 无话说），供 CLI 打印醒目警告
  const std::string& auth_bootstrap_note() const { return auth_bootstrap_note_; }

  // ── SQL 级多库（库 = <data_dir>/<db>.db 一个自包含文件）──
  // 一个引擎实例同一时刻只打开一个库；「当前库」是引擎级状态，
  // 跨库并发用多个引擎实例（多进程）解决。
  const std::string& current_db() const { return current_db_; }
  const std::string& startup_db() const { return startup_db_; }
  // CREATE DATABASE：初始化 <data_dir>/<name>.db（已存在 → DB-514）
  DbStatus CreateDatabase(const std::string& name, std::string* note);
  // DROP DATABASE：软删除（改名 <name>.db.dropped-<时间戳>，可手工改回恢复）。
  // 当前库/启动库不可删（DB-515）；不存在 → DB-514。
  DbStatus DropDatabase(const std::string& name, std::string* note);
  // USE：切换当前库。事务中调用会被会话层拦（DB-513）。
  // 切换 = Close()（顺带把旧库刷盘）→ 换 db_file → Open() → 重载目录。
  DbStatus UseDatabase(const std::string& name, std::string* note);
  // SHOW DATABASES：列 <data_dir>/*.db 的库名（单列查询结果，字母序）
  DbStatus ShowDatabases(QueryResult* out);

  // 诊断文本
  std::string StatsText();
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
  // 身份库：独立于当前库的一份存储实例（USE 切库不影响它）
  std::unique_ptr<storage::IStorage> auth_storage_;
  AuthStore auth_;
  storage::StorageConfig auth_sc_;  // 身份库打开参数（FlushAuth 重开用）
  bool auth_opened_ = false;
  std::string auth_bootstrap_note_;
  std::recursive_mutex storage_mutex_;  // 存储层访问串行化（存储层非线程安全）

  // 存盘点需要重开存储引擎，故保留一份存储层配置
  storage::StorageConfig storage_config_;
  // 存盘点会重建缓冲池（统计计数器清零），故把历史值累计在此，保证观测连续
  storage::BufferStats stats_before_checkpoints_;
  uint32_t checkpoint_count_ = 0;
  // 当前库 / 启动库（库名 = db_file 去掉 .db 后缀）
  std::string current_db_;
  std::string startup_db_;
};

// ── 会话：一条 SQL 执行链路 ─────────────────────────────────
class Session {
 public:
  Session(DbEngine* engine, std::string name);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  const std::string& name() const { return name_; }

  // ── 身份（访问控制）────────────────────────────────────────
  const std::string& user() const { return user_; }
  void SetIdentity(const std::string& user, bool is_admin) {
    user_ = user;
    is_admin_ = is_admin;
  }
  // 认证已启用但尚未登录
  bool need_login() const { return engine_->config().enable_auth && user_.empty(); }
  // 是否按管理员对待（访问控制关闭时一律视为管理员，等价于「无访问控制」）
  bool is_admin() const { return !engine_->config().enable_auth || is_admin_; }
  // ── 访问控制的外部入口 ──
  // 供**直接调用引擎**的层（如 cella_client 的部分端点）复用同一套判定，
  // 否则那些端点会绕过 DDL/库级检查。
  DbStatus RequireAdmin(const char* what) const;
  DbStatus CheckDatabaseAccess(const std::string& db) const;
  void FilterDatabasesByPrivilege(QueryResult* result) const;

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
  // 执行一条用户管理语句（调用前已完成登录检查）；SHOW USERS 会把结果写进 result
  DbStatus ApplyUserCommand(const UserCommand& cmd, std::string* note, QueryResult* result);
  // 执行一条授权语句（调用前已完成登录检查）；SHOW GRANTS 会把结果写进 result
  DbStatus ApplyGrantCommand(const GrantCommand& cmd, std::string* note, QueryResult* result);
  // 访问控制：库级权限（建表/删表）
  DbStatus CheckDbPrivilege(cella::db::Priv need, const char* what) const;

  DbEngine* engine_;
  std::string name_;
  std::string user_;          // 登录用户名（空 = 未登录）
  bool is_admin_ = false;     // 登录身份是否管理员
  txn_id_t txn_ = kInvalidTxnId;
  std::shared_ptr<Transaction> txn_handle_;
  cella::CELLA_Catalog compiler_catalog_;  // 编译器视角的目录（每条语句后与 Catalog 对齐）
};

}  // namespace cella::db
