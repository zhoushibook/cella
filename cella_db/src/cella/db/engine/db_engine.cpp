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

#include <cctype>
#include <ctime>
#include <set>

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

// 库名 = 数据文件名去掉 .db 后缀（"main.db" → "main"）
std::string DbNameFromFile(const std::string& db_file) {
  const std::string suffix = ".db";
  if (db_file.size() > suffix.size() &&
      db_file.compare(db_file.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return db_file.substr(0, db_file.size() - suffix.size());
  }
  return db_file;
}

std::string DbFileOf(const std::string& db_name) { return db_name + ".db"; }

// 库名合法性：标识符规则 [A-Za-z_][A-Za-z0-9_]*（首字符不能是数字），
// 长度 1..64，且不与 SQL 保留字冲突
bool ValidDbName(const std::string& name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  const auto ident_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  if (!ident_start(name[0])) {
    return false;
  }
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
      return false;
    }
  }
  static const char* kReserved[] = {"TABLE",     "DATABASE", "DATABASES", "USE",
                                    "SHOW",      "CREATE",   "DROP",      "GET",
                                    "INSERT",    "DELETE",   "UPDATE",    "SELECT",
                                    "BEGIN",     "COMMIT",   "ROLLBACK"};
  std::string upper;
  upper.reserve(name.size());
  for (const char c : name) {
    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  for (const char* r : kReserved) {
    if (upper == r) {
      return false;
    }
  }
  return true;
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

  // ② 锁管理器
  locks_ = std::make_unique<LockManager>(config_.lock_timeout);

  // ②b 库名与旧布局迁移：库 = <data_dir>/<db>.db 单文件；库名取 db_file 的茎。
  //     db_file 缺少 .db 后缀时补上（--db school → school.db）。
  //     旧版默认数据文件叫 cella.db，首次用新版打开时改名为 main.db（一次性）。
  {
    const std::string suffix = ".db";
    if (config_.db_file.size() <= suffix.size() ||
        config_.db_file.compare(config_.db_file.size() - suffix.size(), suffix.size(), suffix) !=
            0) {
      config_.db_file += suffix;
    }
  }
  current_db_ = DbNameFromFile(config_.db_file);
  startup_db_ = current_db_;
  if (!ValidDbName(current_db_)) {
    return DbStatus::Error(DbCode::kDatabaseError,
                           "数据库名非法: " + current_db_ + "（来自 --db " + config_.db_file + "）");
  }
  {
    const std::string legacy_db = config_.data_dir + "/cella.db";
    const std::string main_db = config_.data_dir + "/" + DbFileOf(current_db_);
    if (current_db_ == "main" && std::filesystem::exists(legacy_db) &&
        !std::filesystem::exists(main_db)) {
      std::error_code rec;
      std::filesystem::rename(legacy_db, main_db, rec);
      if (!rec) {
        DbLogInfo(logcat::kEngine, "旧布局迁移: cella.db → main.db");
      }
    }
  }

  // ③ 存储引擎（保留配置：存盘点需要用同样参数重开）
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
  stats_before_checkpoints_ = storage::BufferStats{};
  checkpoint_count_ = 0;
  storage_ = storage::CreateStorage(sc);
  const storage::Status os = storage_->Open(sc);
  if (!os.ok()) {
    return DbStatus::Error(DbCode::kStorageError, "存储引擎打开失败: " + os.ToString());
  }

  // ④ 系统目录（页式存储里的特殊表 cella_catalog）
  //    目录与数据走同一条持久化路径，不再有「目录/数据文件不一致」的问题。
  catalog_.AttachStorage(storage_.get());
  bool catalog_created = false;
  {
    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
    const DbStatus cs = catalog_.EnsureSystemTable(&catalog_created);
    if (!cs.ok()) {
      return cs;
    }
  }

  // ④b 旧文本目录一次性迁移（catalog.meta → 系统表），迁移完改名留档
  const std::string legacy_meta = config_.data_dir + "/" + CatalogManager::LegacyFileName();
  if (std::filesystem::exists(legacy_meta)) {
    if (catalog_created) {
      // 系统表刚新建（空）→ 解析旧文本并把每张表写进系统表
      const DbStatus ms = catalog_.LoadLegacyText(config_.data_dir);
      if (!ms.ok()) {
        return ms;
      }
      std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
      for (const auto* t : catalog_.ListTables()) {
        // 先确保物理表存在（迁移的旧目录可能只带文本、不带数据文件），
        // 否则 LoadFromStorage 会把「无物理背板」的表当成陈旧条目剔除。
        const DbStatus ps = catalog_.EnsurePhysicalTable(*t);
        if (!ps.ok()) {
          return ps;
        }
        const DbStatus ws = catalog_.WriteTableRow(*t);
        if (!ws.ok()) {
          return ws;
        }
      }
      (void)Checkpoint();  // 迁移结果立即落盘
    }
    std::error_code rec;
    std::filesystem::rename(legacy_meta,
                            config_.data_dir + "/" + CatalogManager::MigratedFileName(), rec);
    if (!rec) {
      DbLogInfo(logcat::kCatalog, "旧目录文件已迁移并改名: " + legacy_meta);
    }
  }

  {
    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
    const DbStatus ls = catalog_.LoadFromStorage();
    if (!ls.ok()) {
      return ls;
    }
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

  // ⑨ 认证启用时立即打开身份库并（必要时）引导管理员 root ——
  //    这样「首次开启认证」的安全提示能在登录之前给出，而不是等第一条语句。
  if (config_.enable_auth) {
    const DbStatus as = EnsureAuthOpen();
    if (!as.ok()) {
      return as;
    }
  }

  opened_ = true;
  DbLogInfo(logcat::kEngine, "引擎已打开: 数据目录=" + config_.data_dir + " 页大小=" +
                                 std::to_string(config_.page_size) + " 缓冲池=" +
                                 std::to_string(config_.pool_size) + " 替换策略=" +
                                 config_.replacer + " 表数=" + std::to_string(catalog_.table_count()));
  return DbStatus::Ok();
}

// ═════════════════════════════════════════════════════════════
// 访问控制：身份库（<data_dir>/cella_auth.db）
// ═════════════════════════════════════════════════════════════

DbStatus DbEngine::EnsureAuthOpen() {
  if (auth_opened_) {
    return DbStatus::Ok();
  }
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kStorageError, "引擎未打开，无法打开身份库");
  }
  // 身份库与当前库共用一套存储参数，但文件独立；USE 切库不会触碰它。
  storage::StorageConfig sc = storage_config_;
  sc.db_file = config_.auth_file.empty() ? AuthStore::kFileName : config_.auth_file;
  auth_storage_ = storage::CreateStorage(sc);
  const storage::Status os = auth_storage_->Open(sc);
  if (!os.ok()) {
    auth_storage_.reset();
    return DbStatus::Error(DbCode::kStorageError, "身份库打开失败: " + os.ToString());
  }
  auth_.AttachStorage(auth_storage_.get());

  std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
  bool created = false;
  const DbStatus et = auth_.EnsureTables(&created);
  if (!et.ok()) {
    return et;
  }
  const DbStatus ls = auth_.LoadFromStorage();
  if (!ls.ok()) {
    return ls;
  }
  auth_opened_ = true;
  DbLogInfo(logcat::kAuth, "身份库已打开: " + config_.data_dir + "/" + sc.db_file);

  // 首次开启认证且库中没有任何用户 → 引导创建管理员 root（空口令），
  // 并留下醒目提示，由 CLI / 服务端打印（避免「装了认证却进不去」）。
  if (config_.enable_auth && auth_.user_count() == 0) {
    std::string note;
    const DbStatus cs = auth_.CreateUser(AuthStore::kDefaultAdmin, "", true, &note);
    if (!cs.ok()) {
      return cs;
    }
    auth_bootstrap_note_ =
        "已创建默认管理员账号 root（口令为空）。请立即用 SET PASSWORD = '新口令'; 修改。";
    DbLogWarn(logcat::kAuth, "引导创建管理员 root（空口令）");
  }
  return DbStatus::Ok();
}

DbStatus DbEngine::Authenticate(const std::string& user, const std::string& password,
                                bool* out_is_admin) {
  const DbStatus os = EnsureAuthOpen();
  if (!os.ok()) {
    return os;
  }
  bool is_admin = false;
  if (!auth_.Authenticate(user, password, &is_admin)) {
    DbLogWarn(logcat::kAuth, "认证失败: " + user);
    // 不区分「无此用户」与「口令错误」，避免用户名枚举
    return DbStatus::Error(DbCode::kAuthFailed, "认证失败：用户名或口令错误");
  }
  if (out_is_admin != nullptr) {
    *out_is_admin = is_admin;
  }
  DbLogInfo(logcat::kAuth, "认证成功: " + user + (is_admin ? "（管理员）" : ""));
  return DbStatus::Ok();
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

// ═════════════════════════════════════════════════════════════
// SQL 级多库：CREATE/DROP DATABASE、USE、SHOW DATABASES
// ═════════════════════════════════════════════════════════════

DbStatus DbEngine::CreateDatabase(const std::string& name, std::string* note) {
  if (!ValidDbName(name)) {
    return DbStatus::Error(DbCode::kDatabaseError,
                           "数据库名非法: \"" + name + "\"（标识符规则，且不与保留字冲突）");
  }
  const std::string file = config_.data_dir + "/" + DbFileOf(name);
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) {
    return DbStatus::Error(DbCode::kDatabaseError, "数据库已存在: " + name);
  }
  // 用一次「打开 + 干净关闭」引导出空数据文件（含存储层目录页）。
  // 系统表 cella_catalog 首次 USE 时由 bootstrap 创建。
  storage::StorageConfig sc = storage_config_;
  sc.db_file = DbFileOf(name);
  std::unique_ptr<storage::IStorage> tmp = storage::CreateStorage(sc);
  const storage::Status os = tmp->Open(sc);
  if (!os.ok()) {
    return DbStatus::Error(DbCode::kStorageError, "创建数据库 " + name + " 失败: " + os.ToString());
  }
  tmp->Close();  // FlushAllPages：把空目录页真正写盘
  if (note != nullptr) {
    *note = "数据库已创建: " + name;
  }
  DbLogInfo(logcat::kEngine, "CREATE DATABASE " + name);
  return DbStatus::Ok();
}

DbStatus DbEngine::DropDatabase(const std::string& name, std::string* note) {
  if (!ValidDbName(name)) {
    return DbStatus::Error(DbCode::kDatabaseError,
                           "数据库名非法: \"" + name + "\"（标识符规则，且不与保留字冲突）");
  }
  if (name == current_db_) {
    return DbStatus::Error(DbCode::kDatabaseProtected,
                           "不能删除当前数据库 " + name + "（先 USE 到别的库）");
  }
  if (name == startup_db_) {
    return DbStatus::Error(DbCode::kDatabaseProtected,
                           "不能删除启动数据库 " + name + "（它是重启后的落点）");
  }
  const std::string file = config_.data_dir + "/" + DbFileOf(name);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    return DbStatus::Error(DbCode::kDatabaseError, "数据库不存在: " + name);
  }
  // 软删除：改名留档而非物理删除，手工改回文件名即可恢复
  const std::string dropped = file + ".dropped-" + std::to_string(NowEpochSeconds());
  std::filesystem::rename(file, dropped, ec);
  if (ec) {
    return DbStatus::Error(DbCode::kStorageError, "删除数据库 " + name + " 失败（改名留档未成功）");
  }
  if (note != nullptr) {
    *note = "数据库已删除（软删除，文件改名 " + std::filesystem::path(dropped).filename().string() +
            "，改回原名即可恢复）: " + name;
  }
  DbLogInfo(logcat::kEngine, "DROP DATABASE " + name);
  return DbStatus::Ok();
}

DbStatus DbEngine::UseDatabase(const std::string& name, std::string* note) {
  if (!ValidDbName(name)) {
    return DbStatus::Error(DbCode::kDatabaseError,
                           "数据库名非法: \"" + name + "\"（标识符规则，且不与保留字冲突）");
  }
  if (name == current_db_) {
    if (note != nullptr) {
      *note = "已经在数据库 " + name + " 中";
    }
    return DbStatus::Ok();
  }
  const std::string file = config_.data_dir + "/" + DbFileOf(name);
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    return DbStatus::Error(DbCode::kDatabaseError, "数据库不存在: " + name);
  }

  // 切换 = 关旧库（Close 内部 FlushAllPages，顺带完成旧库落盘）→ 换 db_file → 开新库。
  // 与 Checkpoint 相同，独占存储互斥量，防止别的线程拿着旧缓冲池的句柄。
  std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
  storage_->Close();
  config_.db_file = DbFileOf(name);
  storage_config_.db_file = config_.db_file;
  const storage::Status s = storage_->Open(storage_config_);
  if (!s.ok()) {
    // 换回旧库文件名再重开，尽量保住引擎可用性
    config_.db_file = DbFileOf(current_db_);
    storage_config_.db_file = config_.db_file;
    (void)storage_->Open(storage_config_);
    return DbStatus::Error(DbCode::kStorageError, "切换到数据库 " + name + " 失败: " + s.ToString());
  }
  catalog_.AttachStorage(storage_.get());
  // 新建的库还没有系统目录表（CreateDatabase 只引导出空数据文件），先走 bootstrap
  const DbStatus cs = catalog_.EnsureSystemTable(nullptr);
  if (!cs.ok()) {
    return cs;
  }
  const DbStatus ls = catalog_.LoadFromStorage();
  if (!ls.ok()) {
    return ls;
  }
  current_db_ = name;
  if (note != nullptr) {
    *note = "已切换到数据库 " + name;
  }
  DbLogInfo(logcat::kEngine, "USE " + name + "（表数 " + std::to_string(catalog_.table_count()) +
                                 "）");
  return DbStatus::Ok();
}

DbStatus DbEngine::ShowDatabases(QueryResult* out) {
  if (out == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "ShowDatabases: 输出为空");
  }
  out->Clear();
  out->columns.push_back(ResultColumn{"name"});
  std::set<std::string> names;
  names.insert(current_db_);  // 当前库一定在（其文件可能刚建尚未落盘）
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(config_.data_dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string fname = entry.path().filename().string();
    const std::string suffix = ".db";
    if (fname.size() <= suffix.size() ||
        fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0) {
      continue;  // 只认 *.db（.db.dropped-* 的软删除留档不会匹配）
    }
    names.insert(fname.substr(0, fname.size() - suffix.size()));
  }
  for (const std::string& n : names) {
    std::vector<storage::Value> row;
    row.push_back(storage::Value::Varchar(n));
    out->rows.push_back(std::move(row));
  }
  out->tag = "SHOW DATABASES " + std::to_string(out->rows.size());
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

  // 身份库是独立的一份存储实例：单独刷盘并释放
  if (auth_storage_) {
    auth_storage_->Close();
  }
  auth_storage_.reset();
  auth_opened_ = false;
  auth_bootstrap_note_.clear();

  if (storage_) {
    storage_->Close();  // FlushAllPages：把脏页（含目录表与目录页）真正写盘
  }
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

// ── 用户管理语句的执行（会话层；调用前已完成登录检查）────────
DbStatus Session::ApplyUserCommand(const UserCommand& cmd, std::string* note, QueryResult* result) {
  using Kind = UserCommand::Kind;
  const bool self_password = (cmd.kind == Kind::kSetPassword) && cmd.name.empty();
  if (!is_admin() && !self_password) {
    return DbStatus::Error(DbCode::kGrantDenied,
                           "仅管理员可以管理用户（新建/删除用户、修改他人口令）");
  }
  if (in_transaction()) {
    return DbStatus::Error(DbCode::kDatabaseTxnActive,
                           "用户管理操作前须结束当前事务（COMMIT / ROLLBACK）");
  }
  const DbStatus os = engine_->EnsureAuthOpen();
  if (!os.ok()) {
    return os;
  }
  std::lock_guard<std::recursive_mutex> guard(engine_->storage_mutex_);
  AuthStore& auth = engine_->auth();

  switch (cmd.kind) {
    case Kind::kCreateUser:
      return auth.CreateUser(cmd.name, cmd.has_password ? cmd.password : std::string(), false, note);
    case Kind::kDropUser:
      if (AuthStore::CanonicalName(cmd.name) == AuthStore::CanonicalName(user_)) {
        return DbStatus::Error(DbCode::kLastAdmin, "不能删除当前登录的用户自己: " + user_);
      }
      return auth.DropUser(cmd.name, note);
    case Kind::kSetPassword: {
      if (!cmd.has_password) {
        return DbStatus::Error(DbCode::kSqlError, "SET PASSWORD 需要新口令");
      }
      const std::string target = cmd.name.empty() ? user_ : cmd.name;
      return auth.SetPassword(target, cmd.password, note);
    }
    case Kind::kShowUsers: {
      if (result != nullptr) {
        result->Clear();
        result->columns.push_back(ResultColumn{"user"});
        result->columns.push_back(ResultColumn{"admin"});
        result->columns.push_back(ResultColumn{"created_at"});
        for (const AuthUser& u : auth.SnapshotUsers()) {
          result->rows.push_back({storage::Value::Varchar(u.name),
                                  storage::Value::Bool(u.is_admin),
                                  storage::Value::BigInt(u.created_at)});
        }
        result->tag = "SHOW USERS " + std::to_string(result->rows.size());
      }
      if (note != nullptr) {
        *note = "SHOW USERS（" + std::to_string(auth.user_count()) + " 个用户）";
      }
      return DbStatus::Ok();
    }
  }
  return DbStatus::Error(DbCode::kInternal, "未知的用户管理语句");
}

DbStatus Session::ExecuteOne(const std::string& stmt_text, int line, int col, StatementOutcome* out) {
  if (out == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "ExecuteOne: out 为空");
  }
  const auto t0 = Clock::now();
  out->sql = stmt_text;
  out->line = line;
  out->col = col;

  // ── ⓪ 登录检查：认证已启用时必须先登录 ──
  if (need_login()) {
    out->status = DbStatus::Error(DbCode::kNoCredentials,
                                  "尚未登录：请提供用户名与口令后再执行语句");
    out->elapsed_ms = MsSince(t0);
    return out->status;
  }

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

  // ── ①b 数据库控制语句（与事务控制同构：编译器不认识，会话层直接执行）──
  // SHOW DATABASES 是只读的，事务中放行；CREATE/DROP/USE 会换库或动文件系统，
  // 事务中一律拒绝（undo 记的是旧库表名，跨库切换会悬空）。
  std::string db_kind;
  std::string db_arg;
  if (IsDatabaseControl(stmt_text, &db_kind, &db_arg)) {
    out->kind = db_kind;
    DbStatus st;
    std::string note;
    if (db_kind != "SHOW DATABASES" && in_transaction()) {
      st = DbStatus::Error(DbCode::kDatabaseTxnActive,
                           db_kind + " 前须结束当前事务（COMMIT / ROLLBACK）");
    } else if (db_kind == "CREATE DATABASE") {
      st = engine_->CreateDatabase(db_arg, &note);
    } else if (db_kind == "DROP DATABASE") {
      st = engine_->DropDatabase(db_arg, &note);
    } else if (db_kind == "USE") {
      if (db_arg.empty()) {
        st = DbStatus::Error(DbCode::kDatabaseError, "USE 缺少数据库名");
      } else {
        st = engine_->UseDatabase(db_arg, &note);
      }
    } else {  // SHOW DATABASES
      st = engine_->ShowDatabases(&out->result);
      note = out->result.tag;
    }
    out->executed = true;
    out->status = st;
    out->result.tag = note;
    out->elapsed_ms = MsSince(t0);
    // USE 换库后权威目录已变，必须重建编译器视角目录，
    // 否则后续语句还在按旧库的表结构做语义分析（静默错乱）。
    EnsureCatalogInSync();
    DbLogInfo(logcat::kSession, "[" + name_ + "] " + db_kind + " " +
                                    (st.ok() ? "OK: " + note : st.ToString()));
    return st;
  }

  // ── ①c 用户管理语句（编译器不认识，会话层直接执行）──
  {
    UserCommand cmd;
    std::string perr;
    const AuthParse pres = ParseUserCommand(stmt_text, &cmd, &perr);
    if (pres == AuthParse::kSyntaxError) {
      out->kind = "USER";
      out->status = DbStatus::Error(DbCode::kSqlError, "语法错误: " + perr);
      out->elapsed_ms = MsSince(t0);
      return out->status;
    }
    if (pres == AuthParse::kOk) {
      std::string note;
      const DbStatus st = ApplyUserCommand(cmd, &note, &out->result);
      out->kind = "USER";
      out->executed = true;
      out->status = st;
      if (st.ok() && out->result.tag.empty()) {
        out->result.tag = note;
      }
      out->elapsed_ms = MsSince(t0);
      DbLogInfo(logcat::kSession,
                "[" + name_ + "] USER " + (st.ok() ? "OK: " + note : st.ToString()));
      return st;
    }
    // kNotAuth → 落到编译器（普通 SQL）
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
  ctx.lock_scope = engine_->current_db();
  // rowid 伪列：只在该语句确实引用它时才让扫描在结果末尾附加（否则 `get *` 会多出一列）
  ctx.with_rowid =
      program->statements.empty() ? false : StmtRefersRowid(program->statements[0].get());

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
      // 自动提交 = 一次真正的事务提交。DDL 必须立刻存盘：目录行和物理表目录
      // 都只是缓冲池里的脏页，进程被强杀会一起丢，下次打开目录里就没这张表。
      // 其余语句按 checkpoint_on_commit 决定。
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
