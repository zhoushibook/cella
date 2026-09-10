#include "cella/db/engine/db_engine.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <sstream>

#include "cella/cella_lexer.h"
#include "cella/cella_optimizer.h"
#include "cella/cella_parser.h"
#include "cella/cella_printer.h"
#include "cella/cella_semantic.h"
#include "cella/db/common/value_bridge.h"
#include "cella/storage/api/storage_factory.h"
#include "cella/storage/common/logger.h"
#include "cella/storage/table/table_heap.h"

namespace cella::db {
namespace {

using Clock = std::chrono::steady_clock;

double MsSince(const Clock::time_point& t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 把编译期诊断拼成一行可读文本
std::string CompileErrorsText(const std::vector<cella::CELLA_Error>& errors) {
  std::ostringstream os;
  for (size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) {
      os << "; ";
    }
    os << cella::cella_errorText(errors[i]);
  }
  return os.str();
}

std::string PlanText(const std::vector<std::unique_ptr<cella::CELLA_PlanNode>>& plans) {
  std::ostringstream os;
  cella::cella_printPlan(plans, os);
  return os.str();
}

// 语句类别名（面向用户的标签）
const char* StmtKindName(cella::CELLA_Stmt::Kind k) {
  switch (k) {
    case cella::CELLA_Stmt::Kind::CREATE_TABLE: return "CREATE TABLE";
    case cella::CELLA_Stmt::Kind::INSERT:       return "INSERT";
    case cella::CELLA_Stmt::Kind::GET:          return "GET";
    case cella::CELLA_Stmt::Kind::DELETE:       return "DELETE";
    case cella::CELLA_Stmt::Kind::UPDATE:       return "UPDATE";
    case cella::CELLA_Stmt::Kind::DROP_TABLE:   return "DROP TABLE";
  }
  return "?";
}

}  // namespace

// ═════════════════════════════════════════════════════════════
// ScriptReport
// ═════════════════════════════════════════════════════════════

void ScriptReport::Tally() {
  ok_count = 0;
  error_count = 0;
  compile_error_count = 0;
  for (const auto& s : statements) {
    if (!s.status.ok()) {
      ++error_count;
    } else if (!s.compile_errors.empty()) {
      ++compile_error_count;
    } else {
      ++ok_count;
    }
  }
}

std::string ScriptReport::ToText(bool verbose) const {
  std::ostringstream os;
  size_t index = 0;
  for (const auto& s : statements) {
    ++index;
    os << "-- 语句 #" << index;
    if (!s.kind.empty()) {
      os << " [" << s.kind << "]";
    }
    if (verbose && (s.line > 0)) {
      os << " @" << s.line << ":" << s.col;
    }
    os << "\n";
    if (verbose && !s.sql.empty()) {
      os << s.sql;
      if (s.sql.back() != '\n') {
        os << "\n";
      }
    }
    if (!s.notice.empty()) {
      os << s.notice << "\n";
    }
    if (!s.compile_errors.empty()) {
      os << CompileErrorsText(s.compile_errors) << "\n";
    }
    if (!s.status.ok() && s.compile_errors.empty()) {
      os << s.status.ToString() << "\n";
    } else if (s.status.ok() && s.executed) {
      if (s.result.IsQuery()) {
        os << s.result.ToText() << "\n";
      } else {
        os << s.result.Summary() << "\n";
      }
    }
    if (verbose) {
      os << "   耗时 " << FormatMillis(s.elapsed_ms) << " / 算子 " << s.operator_calls
         << " 次 / 事务 txn=" << s.txn_id;
      if (s.auto_committed) {
        os << "（自动提交）";
      }
      if (s.rolled_back_here) {
        os << "（语句级回滚）";
      }
      os << "\n";
    }
    if (verbose && !s.plan_text.empty()) {
      os << "   计划:\n";
      std::istringstream in(s.plan_text);
      std::string line;
      while (std::getline(in, line)) {
        os << "     " << line << "\n";
      }
    }
    os << "\n";
  }
  os << "汇总: 成功 " << ok_count << " / 执行失败 " << error_count << " / 编译失败 "
     << compile_error_count;
  return os.str();
}

// ═════════════════════════════════════════════════════════════
// DbEngine
// ═════════════════════════════════════════════════════════════

DbEngine::DbEngine() = default;

DbEngine::~DbEngine() { Close(); }

DbStatus DbEngine::Open(const EngineConfig& config) {
  if (opened_) {
    return DbStatus::Ok();
  }
  config_ = config;

  std::error_code ec;
  std::filesystem::create_directories(config_.data_dir, ec);

  // ① 日志：数据库层写 <data_dir>/cella-db.log，存储层另写 cella-storage.log
  if (config_.enable_log) {
    const std::string path = config_.log_path.empty() ? (config_.data_dir + "/cella-db.log")
                                                      : config_.log_path;
    logger_ = std::make_unique<storage::FileLogger>(path);
    if (config_.log_to_console) {
      console_logger_ = std::make_unique<storage::ConsoleLogger>();
      tee_logger_ = std::make_unique<TeeLogger>(logger_.get(), console_logger_.get());
      DbLogger::Global().Attach(tee_logger_.get());
    } else {
      DbLogger::Global().Attach(logger_.get());
    }
    DbLogger::Global().SetLevel(config_.log_level);
  } else {
    DbLogger::Global().Attach(nullptr);
  }

  // ② 目录（权威元数据）
  const DbStatus cs = catalog_.Load(config_.data_dir);
  if (!cs.ok()) {
    return cs;
  }

  // ③ 锁管理器
  locks_ = std::make_unique<LockManager>(config_.lock_timeout);

  // ④ 存储引擎（保留配置：存盘点需要用同样参数重开）
  storage::StorageConfig sc;
  sc.page_size = config_.page_size;
  sc.pool_size = config_.pool_size;
  sc.replacer = config_.replacer;
  sc.data_dir = config_.data_dir;
  sc.db_file = config_.db_file;
  sc.enable_log = config_.enable_log;
  sc.log_path = config_.data_dir + "/cella-storage.log";
  sc.log_level = config_.log_level;
  storage_config_ = sc;
  recoveries_.clear();
  stats_before_checkpoints_ = storage::BufferStats{};
  checkpoint_count_ = 0;
  storage_ = storage::CreateStorage(sc);
  const storage::Status os = storage_->Open(sc);
  if (!os.ok()) {
    return DbStatus::Error(DbCode::kStorageError, "存储引擎打开失败: " + os.ToString());
  }

  // ⑤ 目录与存储互相校验（不一致时自愈，见 ReconcileCatalogWithStorage 说明）
  const DbStatus rs = ReconcileCatalogWithStorage();
  if (!rs.ok()) {
    return rs;
  }

  // ⑥ 事务管理器（含审计日志）
  txn_manager_ = std::make_unique<TxnManager>(storage_.get(), locks_.get(), &storage_mutex_);
  if (config_.enable_journal) {
    txn_manager_->SetJournalPath(config_.data_dir + "/journal.log");
  }

  // ⑦ 执行器
  executor_ = std::make_unique<Executor>(storage_.get(), &catalog_, txn_manager_.get(), locks_.get(),
                                         &storage_mutex_);

  // ⑧ 默认会话
  default_session_ = std::make_unique<Session>(this, "main");

  opened_ = true;
  DbLogInfo(logcat::kEngine, "引擎已打开: 数据目录=" + config_.data_dir + " 页大小=" +
                                 std::to_string(config_.page_size) + " 缓冲池=" +
                                 std::to_string(config_.pool_size) + " 替换策略=" +
                                 config_.replacer + " 表数=" + std::to_string(catalog_.table_count()));
  return DbStatus::Ok();
}

// ═════════════════════════════════════════════════════════════
// 目录 ↔ 数据文件 的一致性（自愈）
// ═════════════════════════════════════════════════════════════

// 两份元数据的持久性不同：catalog.meta 在 DDL 语句执行时立即原子落盘；
// 而存储层的表目录是一个页，只有 Close() 时才随 FlushAllPages 写出（无 WAL）。
// 一旦进程非正常结束（关终端窗口 / Ctrl-C / 崩溃），就会出现
// 「目录里有表、数据文件里没有」。这里把它修好而不是报错退出：
//   * 按目录里的表结构在数据文件中重建该表（结构保住，原有数据不可恢复）；
//   * 记入 recoveries_，由 CLI 明确提示用户；
//   * 重建后立刻存盘，避免下次打开再走一遍自愈。
DbStatus DbEngine::ReconcileCatalogWithStorage() {
  std::vector<const CatalogTable*> missing;
  for (const auto* t : catalog_.ListTables()) {
    std::shared_ptr<storage::TableHeap> heap;
    const storage::Status s = storage_->open_table(t->name, &heap);
    if (s.ok()) {
      DbLogDebug(logcat::kCatalog, "校验通过: " + t->name + " 首数据页 " +
                                       std::to_string(heap->first_page_id()));
      continue;
    }
    if (s.code() != storage::StatusCode::kTableNotFound) {
      return DbStatus::Error(DbCode::kStorageError,
                             "校验表 " + t->name + " 时存储层返回错误: " + s.ToString());
    }
    missing.push_back(t);
  }
  if (missing.empty()) {
    return DbStatus::Ok();
  }

  for (const auto* t : missing) {
    // 用目录里的列定义重建物理表
    cella::CELLA_Table ct;
    ct.name = t->name;
    for (const auto& c : t->columns) {
      cella::CELLA_Column cc;
      cc.name = c.name;
      cc.type = c.type;
      cc.len = c.len;
      cc.notNull = c.not_null;
      ct.columns.push_back(std::move(cc));
    }
    const storage::Schema schema = ToStorageSchema(ct);
    const storage::Status cs = storage_->create_table(t->name, schema);
    if (!cs.ok()) {
      return DbStatus::Error(DbCode::kStorageError,
                             "重建缺失表 " + t->name + " 失败: " + cs.ToString());
    }
    const std::string note = "目录中的表 " + t->name +
                             " 在数据文件里缺失（上次进程可能未正常关闭），已按目录结构重建；"
                             "该表的原有数据不可恢复，建议用 \\checkpoint 或在正常退出前提交以保"
                             "证落盘";
    recoveries_.push_back(note);
    DbLogWarn(logcat::kCatalog, note);
  }

  // 自愈结果立即落盘，并刷新首数据页等诊断信息
  const DbStatus cp = Checkpoint();
  if (!cp.ok()) {
    DbLogWarn(logcat::kCatalog, "自愈后存盘失败: " + cp.message());
  }
  return DbStatus::Ok();
}

std::string DbEngine::RecoveryReport() const {
  if (recoveries_.empty()) {
    return std::string();
  }
  std::ostringstream os;
  os << "启动时检测到 " << recoveries_.size() << " 处不一致并已自动修复:\n";
  for (const auto& r : recoveries_) {
    os << "  - " << r << "\n";
  }
  return os.str();
}

// ═════════════════════════════════════════════════════════════
// 存盘点
// ═════════════════════════════════════════════════════════════

DbStatus DbEngine::Checkpoint() {
  if (!storage_) {
    return DbStatus::Error(DbCode::kStorageError, "存储引擎未打开，无法存盘");
  }
  // 独占存储互斥量：确保没有任何会话正在访问存储层，
  // 否则重开存储引擎会让别的线程手里的 TableHeap 指向已销毁的缓冲池。
  std::lock_guard<std::recursive_mutex> guard(storage_mutex_);

  // 先把当前缓冲池统计累计下来（重开后计数器会归零）
  const storage::BufferStats before = storage_->get_stats();
  stats_before_checkpoints_.access += before.access;
  stats_before_checkpoints_.hit += before.hit;
  stats_before_checkpoints_.miss += before.miss;
  stats_before_checkpoints_.evict += before.evict;
  stats_before_checkpoints_.dirty_flush += before.dirty_flush;
  stats_before_checkpoints_.disk_reads += before.disk_reads;
  stats_before_checkpoints_.disk_writes += before.disk_writes;
  stats_before_checkpoints_.page_allocs += before.page_allocs;
  stats_before_checkpoints_.page_frees += before.page_frees;

  storage_->Close();  // 内部 FlushAllPages：目录页与脏数据页真正写盘
  const storage::Status s = storage_->Open(storage_config_);
  if (!s.ok()) {
    return DbStatus::Error(DbCode::kStorageError, "存盘点后重开存储引擎失败: " + s.ToString());
  }
  ++checkpoint_count_;
  DbLogInfo(logcat::kEngine, "存盘点完成（第 " + std::to_string(checkpoint_count_) + " 次）");
  return DbStatus::Ok();
}

std::string DbEngine::StatsText() {
  if (!storage_) {
    return "(引擎未打开)";
  }
  std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
  const storage::BufferStats cur = storage_->get_stats();

  // 存盘点会重建缓冲池（计数器归零），这里把历史累计补回去，保证统计连续可读
  storage::BufferStats total;
  total.access = stats_before_checkpoints_.access + cur.access;
  total.hit = stats_before_checkpoints_.hit + cur.hit;
  total.miss = stats_before_checkpoints_.miss + cur.miss;
  total.evict = stats_before_checkpoints_.evict + cur.evict;
  total.dirty_flush = stats_before_checkpoints_.dirty_flush + cur.dirty_flush;
  total.disk_reads = stats_before_checkpoints_.disk_reads + cur.disk_reads;
  total.disk_writes = stats_before_checkpoints_.disk_writes + cur.disk_writes;
  total.page_allocs = stats_before_checkpoints_.page_allocs + cur.page_allocs;
  total.page_frees = stats_before_checkpoints_.page_frees + cur.page_frees;

  std::ostringstream os;
  os << total.ToString() << "\n";
  os << "替换策略: " << config_.replacer << " / 缓冲池帧数: " << config_.pool_size
     << " / 页大小: " << config_.page_size;
  if (checkpoint_count_ != 0) {
    os << " / 存盘点 " << checkpoint_count_ << " 次（上方为累计值）";
  }
  os << "\n";
  const auto evictions = storage_->recent_evictions();
  if (!evictions.empty()) {
    os << "最近淘汰(" << evictions.size() << " 条):\n";
    for (const auto& e : evictions) {
      os << "  " << e << "\n";
    }
  }
  return os.str();
}

void DbEngine::Close() {
  if (!opened_) {
    return;
  }
  // 活动事务一律回滚（避免把未提交的改动留在缓冲池里）
  if (default_session_ && default_session_->in_transaction()) {
    std::string note;
    (void)default_session_->Rollback(&note);
  }
  default_session_.reset();
  executor_.reset();

  if (storage_) {
    storage_->Close();  // FlushAllPages：把脏页（含目录页）真正写盘
  }
  (void)catalog_.Save();
  txn_manager_.reset();
  if (logger_) {
    logger_->Flush();
  }
  DbLogger::Global().Attach(nullptr);
  tee_logger_.reset();
  console_logger_.reset();
  logger_.reset();
  storage_.reset();
  locks_.reset();
  opened_ = false;
}

std::string DbEngine::LockText() const { return locks_ ? locks_->Dump() : "(引擎未打开)"; }

std::string DbEngine::WaitForGraphText() const {
  return locks_ ? locks_->DumpWaitForGraph() : "(引擎未打开)";
}

std::string DbEngine::TxnText() const { return txn_manager_ ? txn_manager_->Dump() : "(引擎未打开)"; }

// ═════════════════════════════════════════════════════════════
// Session
// ═════════════════════════════════════════════════════════════

Session::Session(DbEngine* engine, std::string name) : engine_(engine), name_(std::move(name)) {
  EnsureCatalogInSync();
}

Session::~Session() {
  // 会话结束时未提交的事务必须回滚
  if (in_transaction()) {
    std::string note;
    (void)Rollback(&note);
  }
}

void Session::EnsureCatalogInSync() {
  compiler_catalog_ = engine_->catalog().ToCompilerCatalog();
}

txn_id_t Session::BeginInternal() {
  txn_ = engine_->txn_manager().Begin();
  txn_handle_ = engine_->txn_manager().Find(txn_);
  return txn_;
}

DbStatus Session::Begin(std::string* note) {
  if (in_transaction()) {
    const std::string msg = "已有活动事务 txn=" + std::to_string(txn_);
    if (note != nullptr) {
      *note = msg;
    }
    return DbStatus::Error(DbCode::kTxnAlreadyActive, msg);
  }
  const txn_id_t id = BeginInternal();
  if (note != nullptr) {
    *note = "BEGIN txn=" + std::to_string(id);
  }
  return DbStatus::Ok();
}

DbStatus Session::Commit(std::string* note) {
  if (!in_transaction()) {
    if (note != nullptr) {
      *note = "当前没有活动事务";
    }
    return DbStatus::Error(DbCode::kSessionError, "COMMIT 失败：当前没有活动事务");
  }
  const txn_id_t id = txn_;
  const DbStatus s = engine_->txn_manager().Commit(id);
  txn_ = kInvalidTxnId;
  txn_handle_.reset();
  if (note != nullptr) {
    *note = "COMMIT txn=" + std::to_string(id);
  }
  return s;
}

DbStatus Session::Rollback(std::string* note) {
  if (!in_transaction()) {
    if (note != nullptr) {
      *note = "当前没有活动事务";
    }
    return DbStatus::Error(DbCode::kSessionError, "ROLLBACK 失败：当前没有活动事务");
  }
  const txn_id_t id = txn_;
  const DbStatus s = engine_->txn_manager().Abort(id, "用户 ROLLBACK");
  txn_ = kInvalidTxnId;
  txn_handle_.reset();
  if (note != nullptr) {
    *note = "ROLLBACK txn=" + std::to_string(id);
  }
  return s;
}

std::string Session::StatusLine() const {
  std::ostringstream os;
  os << "会话 " << name_ << " / ";
  if (in_transaction()) {
    os << "事务中 txn=" << txn_;
  } else {
    os << "自动提交模式";
  }
  return os.str();
}

DbStatus Session::Execute(const std::string& sql, ScriptReport* report) {
  if (report == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "Execute: report 为空");
  }
  report->statements.clear();
  if (!engine_->opened()) {
    return DbStatus::Error(DbCode::kInternal, "引擎未打开");
  }

  DbStatus overall = DbStatus::Ok();
  const auto stmts = SplitSqlStatements(sql);
  for (const auto& s : stmts) {
    StatementOutcome out;
    const DbStatus st = ExecuteOne(s.text, s.line, s.col, &out);
    if (!st.ok() && overall.ok()) {
      overall = st;  // 记录第一个失败，便于调用方判断
    }
    report->statements.push_back(std::move(out));
  }
  report->Tally();
  return overall;
}

DbStatus Session::CompileOnly(const std::string& sql, ScriptReport* report) {
  if (report == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "CompileOnly: report 为空");
  }
  report->statements.clear();
  DbStatus overall = DbStatus::Ok();
  const auto stmts = SplitSqlStatements(sql);
  for (const auto& s : stmts) {
    StatementOutcome out;
    out.sql = s.text;
    out.line = s.line;
    out.col = s.col;
    const auto t0 = Clock::now();

    std::vector<cella::CELLA_Error> errors;
    const auto tokens = cella::cella_tokenize(s.text, errors);
    auto program = cella::cella_parse(tokens, errors);
    if (program && !program->statements.empty() && errors.empty()) {
      // 只读语义分析：在目录副本上进行，避免 --show-plan 改变会话目录
      cella::CELLA_Catalog scratch = engine_->catalog().ToCompilerCatalog();
      const auto sem = cella::cella_analyze(*program, scratch);
      errors.insert(errors.end(), sem.errors.begin(), sem.errors.end());
      if (errors.empty()) {
        std::vector<cella::CELLA_Error> plan_errors;
        auto plans = cella::cella_plan(*program, sem.stmtOk, sem.insertColumns, plan_errors);
        errors.insert(errors.end(), plan_errors.begin(), plan_errors.end());
        if (errors.empty() && !plans.empty()) {
          out.kind = StmtKindName(program->statements[0]->kind);
          out.original_plan_text = PlanText(plans);
          out.compiled = true;
          const auto opt = cella::cella_optimizePlans(plans);
          out.plan_text = PlanText(opt.plans);
        }
      }
    }
    out.compile_errors = errors;
    if (!errors.empty()) {
      out.status = DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
      if (overall.ok()) {
        overall = out.status;
      }
    }
    out.elapsed_ms = MsSince(t0);
    report->statements.push_back(std::move(out));
  }
  report->Tally();
  return overall;
}

DbStatus Session::ExecuteOne(const std::string& stmt_text, int line, int col, StatementOutcome* out) {
  if (out == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "ExecuteOne: out 为空");
  }
  const auto t0 = Clock::now();
  out->sql = stmt_text;
  out->line = line;
  out->col = col;

  // ── ① 事务控制语句（编译器不认识 BEGIN/COMMIT/ROLLBACK，在此拦截）──
  std::string keyword;
  if (IsTxnControl(stmt_text, &keyword)) {
    std::string note;
    DbStatus st;
    if (keyword == "BEGIN") {
      st = Begin(&note);
      if (st.ok()) {
        out->txn_id = txn_;
      }
    } else if (keyword == "COMMIT") {
      st = Commit(&note);
      if (st.ok() && engine_->config().checkpoint_on_commit) {
        (void)engine_->Checkpoint();  // 提交即落盘（可选，默认关）
      }
    } else {
      st = Rollback(&note);
    }
    out->executed = true;
    out->status = st;
    out->result.tag = note;
    out->elapsed_ms = MsSince(t0);
    DbLogInfo(logcat::kSession, "[" + name_ + "] " + note);
    return st;
  }

  // ── ② 编译：词法 → 语法 → 语义 → 计划 ──
  std::vector<cella::CELLA_Error> errors;
  const auto tokens = cella::cella_tokenize(stmt_text, errors);
  auto program = cella::cella_parse(tokens, errors);

  if (!program || !errors.empty()) {
    out->compile_errors = errors;
    out->status = DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
    EnsureCatalogInSync();  // 语义阶段可能已改动目录副本 → 复位
    out->elapsed_ms = MsSince(t0);
    return out->status;
  }
  if (program->statements.empty()) {
    out->kind = "(空语句)";
    out->compiled = true;
    out->status = DbStatus::Ok();
    out->elapsed_ms = MsSince(t0);
    return DbStatus::Ok();
  }
  // 语法已通过 → 先确定语句类别，便于编译失败时也能标出是哪类语句
  out->kind = StmtKindName(program->statements[0]->kind);

  const auto sem = cella::cella_analyze(*program, compiler_catalog_);
  errors.insert(errors.end(), sem.errors.begin(), sem.errors.end());

  std::vector<cella::CELLA_Error> plan_errors;
  auto plans = cella::cella_plan(*program, sem.stmtOk, sem.insertColumns, plan_errors);
  errors.insert(errors.end(), plan_errors.begin(), plan_errors.end());

  if (!errors.empty() || plans.empty()) {
    out->compile_errors = errors;
    out->status = DbStatus::Error(
        DbCode::kSqlError,
        errors.empty() ? std::string("未能生成执行计划") : CompileErrorsText(errors));
    EnsureCatalogInSync();
    out->elapsed_ms = MsSince(t0);
    return out->status;
  }

  out->kind = StmtKindName(program->statements[0]->kind);
  out->original_plan_text = PlanText(plans);
  out->compiled = true;

  // ── ③ 计划优化：常量折叠 / 布尔化简 / 恒真 Filter 消除 ──
  // 执行用的是优化后的计划 → 「编译器产出的可执行 IR 直接驱动存储访问」
  const auto opt = cella::cella_optimizePlans(plans);
  out->plan_text = PlanText(opt.plans);
  if (opt.plans.empty()) {
    out->status = DbStatus::Error(DbCode::kInternal, "优化后计划为空");
    out->elapsed_ms = MsSince(t0);
    return out->status;
  }

  // ── ④ 事务上下文：显式事务优先，否则为单语句开自动提交事务 ──
  // DDL 例外：建表/删表会立即改写目录文件（元数据无法回滚），故按 MySQL 惯例
  // 先隐式提交前置事务，再以自动提交方式执行 DDL，避免出现「目录已改、事务回滚」
  // 造成的元数据与数据不一致。
  const bool is_ddl = (out->kind == "CREATE TABLE" || out->kind == "DROP TABLE");
  if (is_ddl && in_transaction()) {
    const txn_id_t prev = txn_;
    std::string note;
    const DbStatus cs = Commit(&note);
    out->implicit_commit = true;
    out->notice = "DDL 隐式提交前置事务 txn=" + std::to_string(prev) +
                  (cs.ok() ? std::string() : std::string("（提交失败）"));
    DbLogInfo(logcat::kSession, out->notice);
  }

  const bool own_txn = !in_transaction();
  if (own_txn) {
    (void)BeginInternal();
  }
  out->auto_committed = own_txn;  // 本条语句是否跑在自建的自动提交事务里
  out->txn_id = txn_;
  const size_t undo_mark = engine_->txn_manager().UndoMark(txn_);
  if (txn_handle_) {
    txn_handle_->AddStatement();
  }

  ExecContext ctx;
  ctx.txn_id = txn_;
  ctx.txn = txn_handle_.get();

  Executor& executor = *engine_->executor_;
  executor.ResetOperatorCalls();
  QueryResult result;
  const DbStatus st = executor.Execute(*opt.plans[0], ctx, &result);
  out->operator_calls = executor.operator_calls();

  // ── ⑤ 收尾：提交 / 语句级回滚 / 整事务回滚 ──
  if (st.ok()) {
    out->executed = true;
    out->result = result;
    if (own_txn) {
      std::string note;
      const DbStatus cs = Commit(&note);
      out->auto_committed = true;
      if (!cs.ok()) {
        out->status = cs;
        out->elapsed_ms = MsSince(t0);
        return cs;
      }
      // 自动提交 = 一次真正的事务提交。DDL 必须立刻存盘，否则进程被强杀时
      // catalog.meta 已写入、数据文件的表目录却没刷出，下次打开会出现
      // 「目录与数据文件不一致」。其余语句按 checkpoint_on_commit 决定。
      if (is_ddl || engine_->config().checkpoint_on_commit) {
        const DbStatus cp = engine_->Checkpoint();
        if (!cp.ok() && is_ddl) {
          DbLogWarn(logcat::kEngine, "DDL 后存盘点失败: " + cp.message());
        }
      }
    }
  } else {
    out->status = st;
    const bool lock_victim = (st.code() == DbCode::kDeadlock || st.code() == DbCode::kLockConflict);
    if (own_txn || lock_victim) {
      // 自动提交失败 → 整个事务回滚；死锁/锁超时牺牲者 → 必须整事务回滚
      std::string note;
      (void)Rollback(&note);
    } else {
      // 显式事务内的普通语句失败：只撤本语句的改动，事务继续（MySQL 风格）
      const DbStatus rs = engine_->txn_manager().RollbackToMark(txn_, undo_mark, st.message());
      out->rolled_back_here = rs.ok();
    }
  }

  // ── ⑥ 目录同步：DDL 已改变权威目录，编译器视角的目录需要跟上 ──
  EnsureCatalogInSync();
  out->elapsed_ms = MsSince(t0);
  return out->status;
}

}  // namespace cella::db
