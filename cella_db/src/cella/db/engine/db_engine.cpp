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

namespace cella::db
{
  namespace
  {

    using Clock = std::chrono::steady_clock;

    double MsSince(const Clock::time_point &t0)
    {
      return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    // 把编译期诊断拼成一行可读文本
    std::string CompileErrorsText(const std::vector<cella::CELLA_Error> &errors)
    {
      std::ostringstream os;
      for (size_t i = 0; i < errors.size(); ++i)
      {
        if (i != 0)
        {
          os << "; ";
        }
        os << cella::cella_errorText(errors[i]);
      }
      return os.str();
    }

    std::string PlanText(const std::vector<std::unique_ptr<cella::CELLA_PlanNode>> &plans)
    {
      std::ostringstream os;
      cella::cella_printPlan(plans, os);
      return os.str();
    }

    // 语句类别名（面向用户的标签）
    const char *StmtKindName(cella::CELLA_Stmt::Kind k)
    {
      switch (k)
      {
      case cella::CELLA_Stmt::Kind::CREATE_TABLE:
        return "CREATE TABLE";
      case cella::CELLA_Stmt::Kind::INSERT:
        return "INSERT";
      case cella::CELLA_Stmt::Kind::GET:
        return "GET";
      case cella::CELLA_Stmt::Kind::DELETE:
        return "DELETE";
      case cella::CELLA_Stmt::Kind::UPDATE:
        return "UPDATE";
      case cella::CELLA_Stmt::Kind::DROP_TABLE:
        return "DROP TABLE";
      case cella::CELLA_Stmt::Kind::CREATE_INDEX:
        return "CREATE INDEX";
      case cella::CELLA_Stmt::Kind::DROP_INDEX:
        return "DROP INDEX";
      }
      return "?";
    }

    // 库名 = 数据文件名去掉 .db 后缀（"main.db" → "main"）
    std::string DbNameFromFile(const std::string &db_file)
    {
      const std::string suffix = ".db";
      if (db_file.size() > suffix.size() &&
          db_file.compare(db_file.size() - suffix.size(), suffix.size(), suffix) == 0)
      {
        return db_file.substr(0, db_file.size() - suffix.size());
      }
      return db_file;
    }

    std::string DbFileOf(const std::string &db_name) { return db_name + ".db"; }

    // 库名合法性：标识符规则 [A-Za-z_][A-Za-z0-9_]*（首字符不能是数字），
    // 长度 1..64，且不与 SQL 保留字冲突
    bool ValidDbName(const std::string &name)
    {
      if (name.empty() || name.size() > 64)
      {
        return false;
      }
      const auto ident_start = [](char c)
      {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
      };
      if (!ident_start(name[0]))
      {
        return false;
      }
      for (const char c : name)
      {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_')
        {
          return false;
        }
      }
      static const char *kReserved[] = {"TABLE", "DATABASE", "DATABASES", "USE",
                                        "SHOW", "CREATE", "DROP", "GET",
                                        "INSERT", "DELETE", "UPDATE", "SELECT",
                                        "BEGIN", "COMMIT", "ROLLBACK"};
      std::string upper;
      upper.reserve(name.size());
      for (const char c : name)
      {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      for (const char *r : kReserved)
      {
        if (upper == r)
        {
          return false;
        }
      }
      return true;
    }

  } // namespace

  // ═════════════════════════════════════════════════════════════
  // ScriptReport
  // ═════════════════════════════════════════════════════════════

  void ScriptReport::Tally()
  {
    ok_count = 0;
    error_count = 0;
    compile_error_count = 0;
    for (const auto &s : statements)
    {
      if (!s.status.ok())
      {
        ++error_count;
      }
      else if (!s.compile_errors.empty())
      {
        ++compile_error_count;
      }
      else
      {
        ++ok_count;
      }
    }
  }

  std::string ScriptReport::ToText(bool verbose) const
  {
    std::ostringstream os;
    size_t index = 0;
    for (const auto &s : statements)
    {
      ++index;
      os << "-- 语句 #" << index;
      if (!s.kind.empty())
      {
        os << " [" << s.kind << "]";
      }
      if (verbose && (s.line > 0))
      {
        os << " @" << s.line << ":" << s.col;
      }
      os << "\n";
      if (verbose && !s.sql.empty())
      {
        os << s.sql;
        if (s.sql.back() != '\n')
        {
          os << "\n";
        }
      }
      if (!s.notice.empty())
      {
        os << s.notice << "\n";
      }
      if (!s.compile_errors.empty())
      {
        os << CompileErrorsText(s.compile_errors) << "\n";
      }
      if (!s.status.ok() && s.compile_errors.empty())
      {
        os << s.status.ToString() << "\n";
      }
      else if (s.status.ok() && s.executed)
      {
        if (s.result.IsQuery())
        {
          os << s.result.ToText() << "\n";
        }
        else
        {
          os << s.result.Summary() << "\n";
        }
      }
      if (verbose)
      {
        os << "   耗时 " << FormatMillis(s.elapsed_ms) << " / 算子 " << s.operator_calls
           << " 次 / 事务 txn=" << s.txn_id;
        if (s.auto_committed)
        {
          os << "（自动提交）";
        }
        if (s.rolled_back_here)
        {
          os << "（语句级回滚）";
        }
        os << "\n";
      }
      if (verbose && !s.plan_text.empty())
      {
        os << "   计划:\n";
        std::istringstream in(s.plan_text);
        std::string line;
        while (std::getline(in, line))
        {
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

  DbStatus DbEngine::Open(const EngineConfig &config)
  {
    if (opened_)
    {
      return DbStatus::Ok();
    }
    config_ = config;

    std::error_code ec;
    std::filesystem::create_directories(config_.data_dir, ec);

    // ① 日志：数据库层写 <data_dir>/cella-db.log，存储层另写 cella-storage.log
    if (config_.enable_log)
    {
      const std::string path = config_.log_path.empty() ? (config_.data_dir + "/cella-db.log")
                                                        : config_.log_path;
      logger_ = std::make_unique<storage::FileLogger>(path);
      if (config_.log_to_console)
      {
        console_logger_ = std::make_unique<storage::ConsoleLogger>();
        tee_logger_ = std::make_unique<TeeLogger>(logger_.get(), console_logger_.get());
        DbLogger::Global().Attach(tee_logger_.get());
      }
      else
      {
        DbLogger::Global().Attach(logger_.get());
      }
      DbLogger::Global().SetLevel(config_.log_level);
    }
    else
    {
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
              0)
      {
        config_.db_file += suffix;
      }
    }
    current_db_ = DbNameFromFile(config_.db_file);
    startup_db_ = current_db_;
    if (!ValidDbName(current_db_))
    {
      return DbStatus::Error(DbCode::kDatabaseError,
                             "数据库名非法: " + current_db_ + "（来自 --db " + config_.db_file + "）");
    }
    {
      const std::string legacy_db = config_.data_dir + "/cella.db";
      const std::string main_db = config_.data_dir + "/" + DbFileOf(current_db_);
      if (current_db_ == "main" && std::filesystem::exists(legacy_db) &&
          !std::filesystem::exists(main_db))
      {
        std::error_code rec;
        std::filesystem::rename(legacy_db, main_db, rec);
        if (!rec)
        {
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
    // 记录「打开前数据文件是否已存在」：首次创建的落盘时机见 ⑩
    const std::string main_db_path = config_.data_dir + "/" + sc.db_file;
    const bool db_file_existed = std::filesystem::exists(main_db_path);
    storage_ = storage::CreateStorage(sc);
    const storage::Status os = storage_->Open(sc);
    if (!os.ok())
    {
      return DbStatus::Error(DbCode::kStorageError, "存储引擎打开失败: " + os.ToString());
    }

    // ④ 系统目录（页式存储里的特殊表 cella_catalog）
    //    目录与数据走同一条持久化路径，不再有「目录/数据文件不一致」的问题。
    catalog_.AttachStorage(storage_.get());
    bool catalog_created = false;
    {
      std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
      const DbStatus cs = catalog_.EnsureSystemTable(&catalog_created);
      if (!cs.ok())
      {
        return cs;
      }
      // ④a 二级索引元数据表（独立系统表 cella_index；P1.2）
      const DbStatus is = catalog_.EnsureIndexTable(nullptr);
      if (!is.ok())
      {
        return is;
      }
    }

    // ④b 旧文本目录一次性迁移（catalog.meta → 系统表），迁移完改名留档
    const std::string legacy_meta = config_.data_dir + "/" + CatalogManager::LegacyFileName();
    if (std::filesystem::exists(legacy_meta))
    {
      if (catalog_created)
      {
        // 系统表刚新建（空）→ 解析旧文本并把每张表写进系统表
        const DbStatus ms = catalog_.LoadLegacyText(config_.data_dir);
        if (!ms.ok())
        {
          return ms;
        }
        std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
        for (const auto *t : catalog_.ListTables())
        {
          // 先确保物理表存在（迁移的旧目录可能只带文本、不带数据文件），
          // 否则 LoadFromStorage 会把「无物理背板」的表当成陈旧条目剔除。
          const DbStatus ps = catalog_.EnsurePhysicalTable(*t);
          if (!ps.ok())
          {
            return ps;
          }
          const DbStatus ws = catalog_.WriteTableRow(*t);
          if (!ws.ok())
          {
            return ws;
          }
        }
        (void)Checkpoint(); // 迁移结果立即落盘
      }
      std::error_code rec;
      std::filesystem::rename(legacy_meta,
                              config_.data_dir + "/" + CatalogManager::MigratedFileName(), rec);
      if (!rec)
      {
        DbLogInfo(logcat::kCatalog, "旧目录文件已迁移并改名: " + legacy_meta);
      }
    }

    {
      std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
      const DbStatus ls = catalog_.LoadFromStorage();
      if (!ls.ok())
      {
        return ls;
      }
      // 索引元数据随目录一并加载（无表 → 视为无索引，非错误）
      const DbStatus ixs = catalog_.LoadIndexesFromStorage();
      if (!ixs.ok())
      {
        return ixs;
      }
    }

    // ⑥ 事务管理器
    txn_manager_ = std::make_unique<TxnManager>(storage_.get(), locks_.get(), &storage_mutex_);

    // ⑦ 执行器
    executor_ = std::make_unique<Executor>(storage_.get(), &catalog_, txn_manager_.get(), locks_.get(),
                                           &storage_mutex_);
    executor_->AttachAuth(&auth_); // 表级权限判定要用（身份库按需打开，指针本身恒定）
    // P1.5：把执行器侧的索引维护挂到事务回滚路径上。回滚只补偿表行，
    // 若不挂钩子，索引会与表数据分叉（重启后仍是脏的）。
    txn_manager_->SetUndoIndexHooks(executor_->undo_adapter());

    // ⑦b WAL + 崩溃恢复（P2）
    // 顺序有讲究：执行器与事务管理器都要写日志，而恢复又要用到它们去回放数据，
    // 所以先把 WAL 装配好、再让恢复去跑。
    {
      const DbStatus ws = OpenWalAndRecover();
      if (!ws.ok())
      {
        return ws;
      }
    }

    // ⑧ 默认会话
    default_session_ = std::make_unique<Session>(this, "main");

    // ⑨ 认证启用时立即打开身份库并（必要时）引导管理员 root ——
    //    这样「首次开启认证」的安全提示能在登录之前给出，而不是等第一条语句。
    if (config_.enable_auth)
    {
      const DbStatus as = EnsureAuthOpen();
      if (!as.ok())
      {
        return as;
      }
    }

    // ⑩ 首次创建即完整落盘（与身份库同一手法，见 FlushAuth 的注释）：新建数据文件时
    //    只有元数据页写盘，目录/系统表页都在缓冲池里；进程若在首次 Close 前被强杀
    //    （web 服务被停、CLI 被 Ctrl+C），会留下「只有元数据页」的半成品 ——
    //    之后每次打开都报 kPageNotFound，整个数据目录报废。这里刷全一次保证自洽。
    if (!db_file_existed)
    {
      std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
      storage_->Close();
      const storage::Status rs = storage_->Open(storage_config_);
      if (!rs.ok())
      {
        return DbStatus::Error(DbCode::kStorageError,
                               "数据文件首次落盘后重开失败: " + rs.ToString());
      }
      catalog_.AttachStorage(storage_.get()); // 对象未换，重挂仅为对齐生命周期语义
    }

    opened_ = true;
    DbLogInfo(logcat::kEngine, "引擎已打开: 数据目录=" + config_.data_dir + " 页大小=" +
                                   std::to_string(config_.page_size) + " 缓冲池=" +
                                   std::to_string(config_.pool_size) + " 替换策略=" +
                                   config_.replacer + " 表数=" + std::to_string(catalog_.table_count()));
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // WAL / 崩溃恢复（P2）
  // ═════════════════════════════════════════════════════════════

  std::string DbEngine::DefaultWalPath() const
  {
    // 按库分文件：切库（USE）时不会把两个库的日志写进同一个文件。
    const std::string stem = DbNameFromFile(config_.db_file);
    return config_.data_dir + "/" + (stem.empty() ? "main" : stem) + ".wal";
  }

  // 早期版本写的是 <data_dir>/journal.log：一份「事务提交后才补一行」的纯文本，
  // 没有任何恢复能力。P2 之后它由 <库名>.wal 取代 —— 这里把老文件改名留档，
  // 既避免误把文本当日志解析，也留下了升级痕迹。
  void DbEngine::RetireLegacyJournal()
  {
    const std::string legacy = config_.data_dir + "/journal.log";
    std::error_code ec;
    if (!std::filesystem::exists(legacy, ec))
    {
      return;
    }
    std::error_code rec;
    std::filesystem::rename(legacy, legacy + ".legacy", rec);
    if (!rec)
    {
      DbLogInfo(logcat::kEngine, "旧版审计日志已退役: journal.log → journal.log.legacy");
    }
  }

  DbStatus DbEngine::OpenWalAndRecover()
  {
    if (!config_.enable_journal)
    {
      wal_.reset();
      wal_path_.clear();
      return DbStatus::Ok();
    }
    RetireLegacyJournal();
    wal_ = std::make_unique<wal::WalManager>();
    wal_->set_flush_each_record(config_.wal_flush_each_record);
    wal_path_ = config_.wal_file.empty() ? DefaultWalPath() : config_.wal_file;
    wal::OpenResult opened;
    const DbStatus ws = wal_->Open(wal_path_, &opened);
    if (!ws.ok())
    {
      return ws;
    }
    // 装配：事务的 begin/commit/abort 由 TxnManager 写，行变更由 Executor 写
    txn_manager_->SetWal(wal_.get());
    executor_->AttachWal(wal_.get());

    if (opened.truncated_bytes != 0)
    {
      DbLogWarn(logcat::kEngine, "发现上次崩溃留下的半截日志（" +
                                     std::to_string(opened.truncated_bytes) + " 字节），已截断");
    }

    wal::RecoveryManager recovery(wal_.get(), txn_manager_.get(), executor_.get());
    const DbStatus rs = recovery.Recover(&recovery_stats_);
    if (!rs.ok())
    {
      return rs;
    }
    if (recovery_stats_.ran)
    {
      DbLogInfo(logcat::kEngine, "崩溃恢复: " + recovery_stats_.ToText());
      // 恢复结果立刻落盘（存盘点把重放出来的脏页写回数据文件），随后把日志整体
      // 清空 —— 恢复出来的状态已是「干净关闭」等价物：已提交改动都落盘、未提交
      // 改动已撤销、没有活动事务，于是日志再没有可重放的内容。下次启动直接打开，
      // 无需再跑一遍恢复（恢复时间与存盘点频率成反比）。
      const DbStatus cs = Checkpoint();
      if (!cs.ok())
      {
        return cs;
      }
      (void)wal_->TruncateAll();
    }
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // 访问控制：身份库（<data_dir>/cella_auth.db）
  // ═════════════════════════════════════════════════════════════

  DbStatus DbEngine::EnsureAuthOpen()
  {
    if (auth_opened_)
    {
      return DbStatus::Ok();
    }
    if (storage_ == nullptr)
    {
      return DbStatus::Error(DbCode::kStorageError, "引擎未打开，无法打开身份库");
    }
    // 身份库与当前库共用一套存储参数，但文件独立；USE 切库不会触碰它。
    storage::StorageConfig sc = storage_config_;
    sc.db_file = config_.auth_file.empty() ? AuthStore::kFileName : config_.auth_file;
    auth_sc_ = sc; // FlushAuth 重开时复用同一份参数
    auth_storage_ = storage::CreateStorage(sc);
    const std::string auth_path = config_.data_dir + "/" + sc.db_file;
    const storage::Status os = auth_storage_->Open(sc);
    if (!os.ok())
    {
      auth_storage_.reset();
      // 身份库是「用户 + 授权」的唯一副本。若它在建库过程中被打断（只写了元数据页、
      // 表页没落盘），之后每次打开都会失败；这里把文件路径与恢复办法一并给出。
      return DbStatus::Error(DbCode::kStorageError,
                             "身份库打开失败: " + os.ToString() + "（文件: " + auth_path +
                                 "）。若该文件已损坏，可把它改名或删除后重新启动 —— "
                                 "会重建身份库并恢复默认管理员 root（口令为空），"
                                 "原有的用户与授权将丢失。");
    }
    auth_.AttachStorage(auth_storage_.get());
    // 写后钩子：每次用户/授权改动都立刻把身份库刷全（见 FlushAuth），
    // 否则改动滞留缓冲池，进程被强杀（Ctrl+C / 关窗口）就丢了 —— 真实踩过的坑。
    auth_.SetFlushHook([this]()
                       { (void)FlushAuth(); });

    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
    bool created = false;
    const DbStatus et = auth_.EnsureTables(&created);
    if (!et.ok())
    {
      return et;
    }
    const DbStatus ls = auth_.LoadFromStorage();
    if (!ls.ok())
    {
      return ls;
    }
    auth_opened_ = true;
    DbLogInfo(logcat::kAuth, "身份库已打开: " + auth_path);

    // 首次开启认证且库中没有任何用户 → 引导创建管理员 root（空口令），
    // 并留下醒目提示，由 CLI / 服务端打印（避免「装了认证却进不去」）。
    if (config_.enable_auth && auth_.user_count() == 0)
    {
      std::string note;
      const DbStatus cs = auth_.CreateUser(AuthStore::kDefaultAdmin, "", true, &note);
      if (!cs.ok())
      {
        return cs;
      }
      auth_bootstrap_note_ =
          "已创建默认管理员账号 root（口令为空）。请立即用 SET PASSWORD = '新口令'; 修改。";
      DbLogWarn(logcat::kAuth, "引导创建管理员 root（空口令）");
    }

    // 首次创建后**立即完整落盘**（放在 bootstrap 之后，让第一次刷出的状态就含 root）。
    // 新建文件时元数据页先写盘、表页只随 Close 刷出；进程若在这中间被强杀，会留下
    // 「只有元数据页」的半成品，之后永远打不开。一次 Close + Open 强制刷全（与存盘点同法），
    // 把窗口压到接近零；此后文件始终至少是自洽的（最坏只丢最后一次改动）。
    if (created)
    {
      auth_storage_->Close();
      const storage::Status rs = auth_storage_->Open(sc);
      if (!rs.ok())
      {
        return DbStatus::Error(DbCode::kStorageError, "身份库首次落盘后重开失败: " + rs.ToString());
      }
    }
    return DbStatus::Ok();
  }

  // 身份库即改即落盘：Close（= FlushAllPages）+ 重开。身份库改动极少（用户/授权管理），
  // 这个代价可以忽略；换来的不变量是「改完即持久」，强杀进程也不丢。
  DbStatus DbEngine::FlushAuth()
  {
    if (auth_storage_ == nullptr)
    {
      return DbStatus::Ok();
    }
    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
    auth_storage_->Close();
    const storage::Status rs = auth_storage_->Open(auth_sc_);
    if (!rs.ok())
    {
      auth_opened_ = false;
      return DbStatus::Error(DbCode::kStorageError, "身份库刷盘后重开失败: " + rs.ToString());
    }
    return DbStatus::Ok();
  }

  DbStatus DbEngine::Authenticate(const std::string &user, const std::string &password,
                                  bool *out_is_admin)
  {
    const DbStatus os = EnsureAuthOpen();
    if (!os.ok())
    {
      return os;
    }
    bool is_admin = false;
    if (!auth_.Authenticate(user, password, &is_admin))
    {
      DbLogWarn(logcat::kAuth, "认证失败: " + user);
      // 不区分「无此用户」与「口令错误」，避免用户名枚举
      return DbStatus::Error(DbCode::kAuthFailed, "认证失败：用户名或口令错误");
    }
    if (out_is_admin != nullptr)
    {
      *out_is_admin = is_admin;
    }
    DbLogInfo(logcat::kAuth, "认证成功: " + user + (is_admin ? "（管理员）" : ""));
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // 存盘点
  // ═════════════════════════════════════════════════════════════

  DbStatus DbEngine::Checkpoint()
  {
    if (!storage_)
    {
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

    // ── ① WAL 规则：脏页落盘之前，日志必须先刷到当前 LSN ──
    if (wal_ != nullptr)
    {
      wal_->Flush();
    }

    storage_->Close(); // 内部 FlushAllPages：目录页与脏数据页真正写盘
    const storage::Status s = storage_->Open(storage_config_);
    if (!s.ok())
    {
      return DbStatus::Error(DbCode::kStorageError, "存盘点后重开存储引擎失败: " + s.ToString());
    }
    ++checkpoint_count_;

    // ── ③ 写 checkpoint 记录：记录此刻仍活动的事务（恢复时据此定 redo 起点）──
    // ── ④ 压缩日志：只保留仍活动事务的记录，其余已经随脏页落盘，不再需要 ──
    if (wal_ != nullptr)
    {
      wal::WalRecord ckpt;
      ckpt.type = wal::RecordType::kCheckpoint;
      if (txn_manager_ != nullptr)
      {
        ckpt.active_txns = txn_manager_->ActiveTxnsWithFirstLsn();
      }
      const wal::lsn_t ckpt_lsn = wal_->Append(ckpt);
      wal_->Flush();
      // keep_from = ckpt_lsn：checkpoint 记录本身要保留 —— 它是「新日志的起点」
      // （ARIES 的截断边界），下一次恢复要靠它定位 redo 起点。只有 lsn 严格小于
      // 它的记录（已随脏页落盘、且不属于任何活动事务）才丢弃。
      // 无活动事务时，这等价于「整段数据记录都可以丢，但 checkpoint 标记留下」。
      wal::lsn_t keep_from = ckpt_lsn;
      for (const auto &kv : ckpt.active_txns)
      {
        if (kv.second != wal::kInvalidLsn && kv.second < keep_from)
        {
          keep_from = kv.second;
        }
      }
      size_t kept = 0;
      size_t dropped = 0;
      const DbStatus xs = wal_->Compact(keep_from, &kept, &dropped);
      if (!xs.ok())
      {
        return xs;
      }
      DbLogInfo(logcat::kEngine, "存盘点完成（第 " + std::to_string(checkpoint_count_) +
                                     " 次）: 日志保留 " + std::to_string(kept) + " 条，丢弃 " +
                                     std::to_string(dropped) + " 条");
      return DbStatus::Ok();
    }
    DbLogInfo(logcat::kEngine, "存盘点完成（第 " + std::to_string(checkpoint_count_) + " 次）");
    return DbStatus::Ok();
  }

  std::string DbEngine::WalText() const
  {
    std::ostringstream os;
    os << (wal_ != nullptr ? wal_->Describe() : std::string("WAL 未启用")) << "\n";
    os << recovery_stats_.ToText();
    return os.str();
  }

  // 模拟强杀：把「崩溃瞬间的磁盘状态」固定下来。
  //
  // 为什么不直接 abort()：真正的崩溃测试需要子进程，而这里要的是**可重复、
  // 可移植、不拖慢测试**的等价物。崩溃丢掉的是「缓冲池里尚未落盘的脏页」，
  // 磁盘上留下的是「上次存盘点 + 期间被淘汰出去的页」，WAL 里留下的是
  // 「按刷盘策略已经交给 OS 的那些记录」。把这三者此刻的字节原样复制出来，
  // 再用它覆盖工作目录，重新打开面对的就是一模一样的现场。
  DbStatus DbEngine::SimulateCrash()
  {
    if (!opened_)
    {
      return DbStatus::Error(DbCode::kInternal, "引擎未打开，无法模拟崩溃");
    }
    std::error_code ec;
    const std::string snap_dir = config_.data_dir + "/__crash_snapshot";
    std::filesystem::remove_all(snap_dir, ec);
    std::filesystem::create_directories(snap_dir, ec);

    const std::string db_path = config_.data_dir + "/" + config_.db_file;
    // 关键：wal_path_ 在下面的 Close() 里会被清空，所以必须在 Close 之前
    // 把 WAL 路径抓一份下来，否则还原时就拿不到正确的文件名（空串会把
    // 快照目录错误地覆盖到数据目录上，WAL 等于没恢复）。
    const std::string wal_path = wal_path_;
    auto snapshot = [&](const std::string &src) -> bool
    {
      if (src.empty() || !std::filesystem::exists(src, ec))
      {
        return false;
      }
      const std::string dst = snap_dir + "/" + std::filesystem::path(src).filename().string();
      std::error_code cec;
      std::filesystem::copy_file(src, dst,
                                 std::filesystem::copy_options::overwrite_existing, cec);
      return !cec;
    };
    const bool snapped_db = snapshot(db_path);
    const bool snapped_wal = snapshot(wal_path);

    // 正常关闭（里面会刷页、回滚、清日志），随后用崩溃快照覆盖回去 ——
    // 这样既不会泄漏文件句柄，也不会把「正常关闭」的副作用带进崩溃现场。
    Close();

    auto restore = [&](const std::string &src, bool ok) -> bool
    {
      if (!ok)
      {
        return false;
      }
      const std::string dst = config_.data_dir + "/" + std::filesystem::path(src).filename().string();
      std::error_code rec;
      std::filesystem::copy_file(src, dst,
                                 std::filesystem::copy_options::overwrite_existing, rec);
      return !rec;
    };
    restore(snap_dir + "/" + std::filesystem::path(db_path).filename().string(), snapped_db);
    if (snapped_wal)
    {
      restore(snap_dir + "/" + std::filesystem::path(wal_path).filename().string(), true);
    }
    // 身份库不参与崩溃断言，快照里没有它 —— 保持原样即可。
    DbLogWarn(logcat::kEngine, "已模拟进程强杀（崩溃快照: " + snap_dir + "）");
    return DbStatus::Ok();
  }

  // ═════════════════════════════════════════════════════════════
  // SQL 级多库：CREATE/DROP DATABASE、USE、SHOW DATABASES
  // ═════════════════════════════════════════════════════════════

  DbStatus DbEngine::CreateDatabase(const std::string &name, std::string *note)
  {
    if (!ValidDbName(name))
    {
      return DbStatus::Error(DbCode::kDatabaseError,
                             "数据库名非法: \"" + name + "\"（标识符规则，且不与保留字冲突）");
    }
    if (AuthStore::CanonicalName(name) == AuthStore::CanonicalName(AuthStore::kReservedDbName))
    {
      return DbStatus::Error(DbCode::kDatabaseError,
                             "数据库名 " + name + " 是保留名（身份库专用，不属于用户库）");
    }
    const std::string file = config_.data_dir + "/" + DbFileOf(name);
    std::error_code ec;
    if (std::filesystem::exists(file, ec))
    {
      return DbStatus::Error(DbCode::kDatabaseError, "数据库已存在: " + name);
    }
    // 用一次「打开 + 干净关闭」引导出空数据文件（含存储层目录页）。
    // 系统表 cella_catalog 首次 USE 时由 bootstrap 创建。
    storage::StorageConfig sc = storage_config_;
    sc.db_file = DbFileOf(name);
    std::unique_ptr<storage::IStorage> tmp = storage::CreateStorage(sc);
    const storage::Status os = tmp->Open(sc);
    if (!os.ok())
    {
      return DbStatus::Error(DbCode::kStorageError, "创建数据库 " + name + " 失败: " + os.ToString());
    }
    tmp->Close(); // FlushAllPages：把空目录页真正写盘
    if (note != nullptr)
    {
      *note = "数据库已创建: " + name;
    }
    DbLogInfo(logcat::kEngine, "CREATE DATABASE " + name);
    return DbStatus::Ok();
  }

  DbStatus DbEngine::DropDatabase(const std::string &name, std::string *note)
  {
    if (!ValidDbName(name))
    {
      return DbStatus::Error(DbCode::kDatabaseError,
                             "数据库名非法: \"" + name + "\"（标识符规则，且不与保留字冲突）");
    }
    if (name == current_db_)
    {
      return DbStatus::Error(DbCode::kDatabaseProtected,
                             "不能删除当前数据库 " + name + "（先 USE 到别的库）");
    }
    if (name == startup_db_)
    {
      return DbStatus::Error(DbCode::kDatabaseProtected,
                             "不能删除启动数据库 " + name + "（它是重启后的落点）");
    }
    const std::string file = config_.data_dir + "/" + DbFileOf(name);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
      return DbStatus::Error(DbCode::kDatabaseError, "数据库不存在: " + name);
    }
    // 软删除：改名留档而非物理删除，手工改回文件名即可恢复
    const std::string dropped = file + ".dropped-" + std::to_string(NowEpochSeconds());
    std::filesystem::rename(file, dropped, ec);
    if (ec)
    {
      return DbStatus::Error(DbCode::kStorageError, "删除数据库 " + name + " 失败（改名留档未成功）");
    }
    if (note != nullptr)
    {
      *note = "数据库已删除（软删除，文件改名 " + std::filesystem::path(dropped).filename().string() +
              "，改回原名即可恢复）: " + name;
    }
    DbLogInfo(logcat::kEngine, "DROP DATABASE " + name);
    return DbStatus::Ok();
  }

  DbStatus DbEngine::UseDatabase(const std::string &name, std::string *note)
  {
    if (!ValidDbName(name))
    {
      return DbStatus::Error(DbCode::kDatabaseError,
                             "数据库名非法: \"" + name + "\"（标识符规则，且不与保留字冲突）");
    }
    if (name == current_db_)
    {
      if (note != nullptr)
      {
        *note = "已经在数据库 " + name + " 中";
      }
      return DbStatus::Ok();
    }
    const std::string file = config_.data_dir + "/" + DbFileOf(name);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
      return DbStatus::Error(DbCode::kDatabaseError, "数据库不存在: " + name);
    }

    // 切换 = 关旧库（Close 内部 FlushAllPages，顺带完成旧库落盘）→ 换 db_file → 开新库。
    // 与 Checkpoint 相同，独占存储互斥量，防止别的线程拿着旧缓冲池的句柄。
    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
    // 旧库的日志先收尾：页马上要全部落盘，之后这段日志就没有用了。
    if (wal_ != nullptr)
    {
      wal_->Flush();
    }
    storage_->Close();
    if (wal_ != nullptr)
    {
      (void)wal_->TruncateAll();
      wal_.reset();
    }
    wal_path_.clear();
    config_.db_file = DbFileOf(name);
    storage_config_.db_file = config_.db_file;
    const storage::Status s = storage_->Open(storage_config_);
    if (!s.ok())
    {
      // 换回旧库文件名再重开，尽量保住引擎可用性
      config_.db_file = DbFileOf(current_db_);
      storage_config_.db_file = config_.db_file;
      (void)storage_->Open(storage_config_);
      return DbStatus::Error(DbCode::kStorageError, "切换到数据库 " + name + " 失败: " + s.ToString());
    }
    catalog_.AttachStorage(storage_.get());
    // 新建的库还没有系统目录表（CreateDatabase 只引导出空数据文件），先走 bootstrap
    const DbStatus cs = catalog_.EnsureSystemTable(nullptr);
    if (!cs.ok())
    {
      return cs;
    }
    const DbStatus xs = catalog_.EnsureIndexTable(nullptr);
    if (!xs.ok())
    {
      return xs;
    }
    const DbStatus ls = catalog_.LoadFromStorage();
    if (!ls.ok())
    {
      return ls;
    }
    const DbStatus ixs = catalog_.LoadIndexesFromStorage();
    if (!ixs.ok())
    {
      return ixs;
    }
    current_db_ = name;
    // WAL 按库分文件 → 打开新库的日志，并顺带跑一次崩溃恢复
    // （新库上次也可能是被强杀的）。
    const DbStatus ws = OpenWalAndRecover();
    if (!ws.ok())
    {
      return ws;
    }
    if (note != nullptr)
    {
      *note = "已切换到数据库 " + name;
    }
    DbLogInfo(logcat::kEngine, "USE " + name + "（表数 " + std::to_string(catalog_.table_count()) +
                                   "）");
    return DbStatus::Ok();
  }

  DbStatus DbEngine::ShowDatabases(QueryResult *out)
  {
    if (out == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "ShowDatabases: 输出为空");
    }
    out->Clear();
    out->columns.push_back(ResultColumn{"name"});
    std::set<std::string> names;
    names.insert(current_db_); // 当前库一定在（其文件可能刚建尚未落盘）
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(config_.data_dir, ec))
    {
      if (!entry.is_regular_file(ec))
      {
        continue;
      }
      const std::string fname = entry.path().filename().string();
      const std::string suffix = ".db";
      if (fname.size() <= suffix.size() ||
          fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0)
      {
        continue; // 只认 *.db（.db.dropped-* 的软删除留档不会匹配）
      }
      const std::string stem = fname.substr(0, fname.size() - suffix.size());
      if (stem == AuthStore::kReservedDbName)
      {
        continue; // 身份库不是用户库，不出现在列表里
      }
      names.insert(stem);
    }
    for (const std::string &n : names)
    {
      std::vector<storage::Value> row;
      row.push_back(storage::Value::Varchar(n));
      out->rows.push_back(std::move(row));
    }
    out->tag = "SHOW DATABASES " + std::to_string(out->rows.size());
    return DbStatus::Ok();
  }

  DbStatus DbEngine::ShowIndexes(const std::string &table, QueryResult *out)
  {
    if (out == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "ShowIndexes: 输出为空");
    }
    out->Clear();
    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);

    std::vector<const CatalogIndex *> found;
    if (table.empty())
    {
      found = catalog_.ListIndexes();
    }
    else
    {
      if (catalog_.FindTable(table) == nullptr)
      {
        return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + table);
      }
      found = catalog_.IndexesOfTable(table);
    }
    // 表头必须先建：客户端靠 columns 非空来判定「这是一条查询」
    // （QueryResult::IsQuery），缺了会导致 JSON 里 columns 为 null。
    out->columns.push_back(ResultColumn{"index_name"});
    out->columns.push_back(ResultColumn{"table_name"});
    out->columns.push_back(ResultColumn{"column_name"});
    out->columns.push_back(ResultColumn{"unique"});
    out->columns.push_back(ResultColumn{"root_page_id"});
    for (const CatalogIndex *ix : found)
    {
      std::vector<storage::Value> row;
      row.push_back(storage::Value::Varchar(ix->name));
      row.push_back(storage::Value::Varchar(ix->table));
      row.push_back(storage::Value::Varchar(ix->column));
      row.push_back(storage::Value::Int(ix->unique ? 1 : 0));
      row.push_back(storage::Value::Int(static_cast<int32_t>(ix->root_page_id)));
      out->rows.push_back(std::move(row));
    }
    out->tag = "SHOW INDEXES " + std::to_string(out->rows.size());
    return DbStatus::Ok();
  }

  DbStatus DbEngine::Explain(const std::string &sql, QueryResult *out)
  {
    if (out == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Explain: 输出为空");
    }
    out->Clear();
    if (executor_ == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "引擎未打开");
    }

    // 编译（与 CompileOnly 同一条链路，且用目录副本，绝不改动会话目录）
    std::vector<cella::CELLA_Error> errors;
    const auto tokens = cella::cella_tokenize(sql, errors);
    auto program = cella::cella_parse(tokens, errors);
    if (!program || program->statements.empty() || !errors.empty())
    {
      return DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
    }
    cella::CELLA_Catalog scratch = catalog_.ToCompilerCatalog();
    const auto sem = cella::cella_analyze(*program, scratch);
    errors.insert(errors.end(), sem.errors.begin(), sem.errors.end());
    if (!errors.empty())
    {
      return DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
    }
    std::vector<cella::CELLA_Error> plan_errors;
    auto plans = cella::cella_plan(*program, sem.stmtOk, sem.insertColumns, plan_errors);
    errors.insert(errors.end(), plan_errors.begin(), plan_errors.end());
    if (!errors.empty() || plans.empty())
    {
      return DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
    }
    const auto opt = cella::cella_optimizePlans(plans);
    if (opt.plans.empty())
    {
      return DbStatus::Error(DbCode::kInternal, "Explain: 优化后计划为空");
    }

    std::lock_guard<std::recursive_mutex> guard(storage_mutex_);
    // 输出用单列结果，复用既有的表格渲染通道（客户端据「有列名」判定为查询）
    out->columns.clear();
    ResultColumn col;
    col.name = "plan";
    out->columns.push_back(std::move(col));

    // ① 计划骨架：与 `-p` 走同一条打印路径，因此这一段与 golden 完全同源
    {
      std::ostringstream os;
      cella::cella_printPlan(opt.plans, os);
      out->rows.push_back({storage::Value::Varchar(os.str())});
    }
    // ② 访问路径选择树：表访问节点显示真正选中的路径（索引/全表/rowid 直达）
    for (const auto &plan : opt.plans)
    {
      out->rows.push_back({storage::Value::Varchar(executor_->ExplainText(*plan))});
    }
    return DbStatus::Ok();
  }

  std::string DbEngine::StatsText()
  {
    if (!storage_)
    {
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
    if (checkpoint_count_ != 0)
    {
      os << " / 存盘点 " << checkpoint_count_ << " 次（上方为累计值）";
    }
    os << "\n";
    const auto evictions = storage_->recent_evictions();
    if (!evictions.empty())
    {
      os << "最近淘汰(" << evictions.size() << " 条):\n";
      for (const auto &e : evictions)
      {
        os << "  " << e << "\n";
      }
    }
    return os.str();
  }

  void DbEngine::Close()
  {
    if (!opened_)
    {
      return;
    }
    // 活动事务一律回滚（避免把未提交的改动留在缓冲池里）
    if (default_session_ && default_session_->in_transaction())
    {
      std::string note;
      (void)default_session_->Rollback(&note);
    }
    default_session_.reset();
    executor_.reset();

    // 身份库是独立的一份存储实例：单独刷盘并释放
    if (auth_storage_)
    {
      auth_storage_->Close();
    }
    auth_storage_.reset();
    auth_opened_ = false;
    auth_bootstrap_note_.clear();

    if (storage_)
    {
      // 干净关闭：先把日志刷下去（顺序不能反 —— 页里的改动必须先在日志里有据可查），
      storage_->Close(); // FlushAllPages：把脏页（含目录表与目录页）真正写盘
    }
    // 再清空日志：数据已经全部落盘，下次启动无需重放任何东西。
    // 这正是「恢复时间与存盘点频率成反比」在干净关闭时的极端情形 —— 时间为零。
    if (wal_ != nullptr)
    {
      (void)wal_->TruncateAll();
      wal_.reset();
    }
    wal_path_.clear();
    txn_manager_.reset();
    if (logger_)
    {
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

  std::string DbEngine::WaitForGraphText() const
  {
    return locks_ ? locks_->DumpWaitForGraph() : "(引擎未打开)";
  }

  std::string DbEngine::TxnText() const { return txn_manager_ ? txn_manager_->Dump() : "(引擎未打开)"; }

  // ═════════════════════════════════════════════════════════════
  // Session
  // ═════════════════════════════════════════════════════════════

  Session::Session(DbEngine *engine, std::string name) : engine_(engine), name_(std::move(name))
  {
    EnsureCatalogInSync();
  }

  Session::~Session()
  {
    // 会话结束时未提交的事务必须回滚
    if (in_transaction())
    {
      std::string note;
      (void)Rollback(&note);
    }
  }

  void Session::EnsureCatalogInSync()
  {
    compiler_catalog_ = engine_->catalog().ToCompilerCatalog();
  }

  txn_id_t Session::BeginInternal()
  {
    txn_ = engine_->txn_manager().Begin();
    txn_handle_ = engine_->txn_manager().Find(txn_);
    return txn_;
  }

  DbStatus Session::Begin(std::string *note)
  {
    if (in_transaction())
    {
      const std::string msg = "已有活动事务 txn=" + std::to_string(txn_);
      if (note != nullptr)
      {
        *note = msg;
      }
      return DbStatus::Error(DbCode::kTxnAlreadyActive, msg);
    }
    const txn_id_t id = BeginInternal();
    if (note != nullptr)
    {
      *note = "BEGIN txn=" + std::to_string(id);
    }
    return DbStatus::Ok();
  }

  DbStatus Session::Commit(std::string *note)
  {
    if (!in_transaction())
    {
      if (note != nullptr)
      {
        *note = "当前没有活动事务";
      }
      return DbStatus::Error(DbCode::kSessionError, "COMMIT 失败：当前没有活动事务");
    }
    if (txn_handle_ != nullptr && txn_handle_->rollback_failed())
    {
      return DbStatus::Error(DbCode::kTxnAborted,
                             "COMMIT 失败：事务回滚未完成，请重试 ROLLBACK");
    }
    const txn_id_t id = txn_;
    const DbStatus s = engine_->txn_manager().Commit(id);
    txn_ = kInvalidTxnId;
    txn_handle_.reset();
    if (note != nullptr)
    {
      *note = "COMMIT txn=" + std::to_string(id);
    }
    return s;
  }

  DbStatus Session::Rollback(std::string *note)
  {
    if (!in_transaction())
    {
      if (note != nullptr)
      {
        *note = "当前没有活动事务";
      }
      return DbStatus::Error(DbCode::kSessionError, "ROLLBACK 失败：当前没有活动事务");
    }
    const txn_id_t id = txn_;
    const DbStatus s = engine_->txn_manager().Abort(id, "用户 ROLLBACK");
    if (s.ok())
    {
      txn_ = kInvalidTxnId;
      txn_handle_.reset();
    }
    if (note != nullptr)
    {
      *note = "ROLLBACK txn=" + std::to_string(id);
    }
    return s;
  }

  std::string Session::StatusLine() const
  {
    std::ostringstream os;
    os << "会话 " << name_ << " / ";
    if (in_transaction())
    {
      os << "事务中 txn=" << txn_;
    }
    else
    {
      os << "自动提交模式";
    }
    return os.str();
  }

  DbStatus Session::Execute(const std::string &sql, ScriptReport *report)
  {
    if (report == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "Execute: report 为空");
    }
    report->statements.clear();
    if (!engine_->opened())
    {
      return DbStatus::Error(DbCode::kInternal, "引擎未打开");
    }
    if (in_transaction() && txn_handle_ != nullptr && txn_handle_->rollback_failed())
    {
      const std::string upper = cella::db::TrimUpper(sql);
      if (upper != "ROLLBACK;" && upper != "ROLLBACK")
      {
        return DbStatus::Error(DbCode::kTxnAborted,
                               "事务回滚未完成，只允许重试 ROLLBACK");
      }
    }

    DbStatus overall = DbStatus::Ok();
    const auto stmts = SplitSqlStatements(sql);
    for (const auto &s : stmts)
    {
      StatementOutcome out;
      const DbStatus st = ExecuteOne(s.text, s.line, s.col, &out);
      if (!st.ok() && overall.ok())
      {
        overall = st; // 记录第一个失败，便于调用方判断
      }
      report->statements.push_back(std::move(out));
    }
    report->Tally();
    return overall;
  }

  DbStatus Session::CompileOnly(const std::string &sql, ScriptReport *report)
  {
    if (report == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "CompileOnly: report 为空");
    }
    report->statements.clear();
    DbStatus overall = DbStatus::Ok();
    const auto stmts = SplitSqlStatements(sql);
    for (const auto &s : stmts)
    {
      StatementOutcome out;
      out.sql = s.text;
      out.line = s.line;
      out.col = s.col;
      const auto t0 = Clock::now();

      std::vector<cella::CELLA_Error> errors;
      const auto tokens = cella::cella_tokenize(s.text, errors);
      auto program = cella::cella_parse(tokens, errors);
      if (program && !program->statements.empty() && errors.empty())
      {
        // 只读语义分析：在目录副本上进行，避免 --show-plan 改变会话目录
        cella::CELLA_Catalog scratch = engine_->catalog().ToCompilerCatalog();
        const auto sem = cella::cella_analyze(*program, scratch);
        errors.insert(errors.end(), sem.errors.begin(), sem.errors.end());
        if (errors.empty())
        {
          std::vector<cella::CELLA_Error> plan_errors;
          auto plans = cella::cella_plan(*program, sem.stmtOk, sem.insertColumns, plan_errors);
          errors.insert(errors.end(), plan_errors.begin(), plan_errors.end());
          if (errors.empty() && !plans.empty())
          {
            out.kind = StmtKindName(program->statements[0]->kind);
            out.original_plan_text = PlanText(plans);
            out.compiled = true;
            const auto opt = cella::cella_optimizePlans(plans);
            out.plan_text = PlanText(opt.plans);
          }
        }
      }
      out.compile_errors = errors;
      if (!errors.empty())
      {
        out.status = DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
        if (overall.ok())
        {
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
  DbStatus Session::ApplyUserCommand(const UserCommand &cmd, std::string *note, QueryResult *result)
  {
    using Kind = UserCommand::Kind;
    const bool self_password = (cmd.kind == Kind::kSetPassword) && cmd.name.empty();
    if (!is_admin() && !self_password)
    {
      return DbStatus::Error(DbCode::kGrantDenied,
                             "仅管理员可以管理用户（新建/删除用户、修改他人口令）");
    }
    if (in_transaction())
    {
      return DbStatus::Error(DbCode::kDatabaseTxnActive,
                             "用户管理操作前须结束当前事务（COMMIT / ROLLBACK）");
    }
    const DbStatus os = engine_->EnsureAuthOpen();
    if (!os.ok())
    {
      return os;
    }
    std::lock_guard<std::recursive_mutex> guard(engine_->storage_mutex_);
    AuthStore &auth = engine_->auth();

    switch (cmd.kind)
    {
    case Kind::kCreateUser:
      return auth.CreateUser(cmd.name, cmd.has_password ? cmd.password : std::string(), false, note);
    case Kind::kDropUser:
      if (AuthStore::CanonicalName(cmd.name) == AuthStore::CanonicalName(user_))
      {
        return DbStatus::Error(DbCode::kLastAdmin, "不能删除当前登录的用户自己: " + user_);
      }
      return auth.DropUser(cmd.name, note);
    case Kind::kSetPassword:
    {
      if (!cmd.has_password)
      {
        return DbStatus::Error(DbCode::kSqlError, "SET PASSWORD 需要新口令");
      }
      const std::string target = cmd.name.empty() ? user_ : cmd.name;
      return auth.SetPassword(target, cmd.password, note);
    }
    case Kind::kShowUsers:
    {
      if (result != nullptr)
      {
        result->Clear();
        result->columns.push_back(ResultColumn{"user"});
        result->columns.push_back(ResultColumn{"admin"});
        result->columns.push_back(ResultColumn{"created_at"});
        for (const AuthUser &u : auth.SnapshotUsers())
        {
          result->rows.push_back({storage::Value::Varchar(u.name),
                                  storage::Value::Bool(u.is_admin),
                                  storage::Value::BigInt(u.created_at)});
        }
        result->tag = "SHOW USERS " + std::to_string(result->rows.size());
      }
      if (note != nullptr)
      {
        *note = "SHOW USERS（" + std::to_string(auth.user_count()) + " 个用户）";
      }
      return DbStatus::Ok();
    }
    }
    return DbStatus::Error(DbCode::kInternal, "未知的用户管理语句");
  }

  // ── 授权语句的执行（会话层；调用前已完成登录检查）────────────
  DbStatus Session::ApplyGrantCommand(const GrantCommand &cmd, std::string *note, QueryResult *result)
  {
    using Kind = GrantCommand::Kind;
    const bool show = (cmd.kind == Kind::kShowGrants);
    if (!is_admin() && !show)
    {
      return DbStatus::Error(DbCode::kGrantDenied, "只有管理员可以授予或撤销权限");
    }
    if (!show && in_transaction())
    {
      return DbStatus::Error(DbCode::kDatabaseTxnActive,
                             "授权操作前须结束当前事务（COMMIT / ROLLBACK）");
    }
    const DbStatus os = engine_->EnsureAuthOpen();
    if (!os.ok())
    {
      return os;
    }
    AuthStore &auth = engine_->auth();

    if (show)
    {
      const std::string target = cmd.name.empty() ? user_ : cmd.name;
      if (!is_admin() && AuthStore::CanonicalName(target) != AuthStore::CanonicalName(user_))
      {
        return DbStatus::Error(DbCode::kGrantDenied, "只能查看自己的授权（管理员可查看他人）");
      }
      if (!auth.HasUser(target))
      {
        return DbStatus::Error(DbCode::kUserError, "用户不存在: " + target);
      }
      if (result != nullptr)
      {
        result->Clear();
        result->columns.push_back(ResultColumn{"scope"});
        result->columns.push_back(ResultColumn{"priv"});
        for (const AuthGrant &g : auth.GrantsOf(target))
        {
          result->rows.push_back({storage::Value::Varchar(FormatScope(g.scope_db, g.scope_table)),
                                  storage::Value::Varchar(PrivName(g.priv))});
        }
        if (auth.IsAdmin(target))
        { // 管理员是用户属性，不在权限行里，这里补一行说明
          result->rows.push_back(
              {storage::Value::Varchar("*.*"), storage::Value::Varchar(PrivName(Priv::kAdmin))});
        }
        result->tag = "SHOW GRANTS " + std::to_string(result->rows.size());
      }
      if (note != nullptr)
      {
        *note = "SHOW GRANTS FOR " + target;
      }
      return DbStatus::Ok();
    }

    // 作用域：未写库名 → 当前库；写了 `*` → 全局
    const std::string db = cmd.scope_db.empty() ? engine_->current_db() : cmd.scope_db;
    const std::string tbl = cmd.scope_table.empty() ? "*" : cmd.scope_table;
    std::lock_guard<std::recursive_mutex> guard(engine_->storage_mutex_);
    for (const std::string &raw : cmd.privs)
    {
      Priv priv;
      if (!ParsePriv(raw, &priv))
      {
        return DbStatus::Error(DbCode::kSqlError,
                               "未知权限名: " + raw +
                                   "（可用 get / insert / update / delete / create / drop / all / admin）");
      }
      for (const std::string &u : cmd.users)
      {
        DbStatus st;
        if (priv == Priv::kAdmin)
        {
          if (cmd.kind == Kind::kGrant)
          {
            st = auth.SetAdmin(u, true, note);
          }
          else
          {
            if (AuthStore::CanonicalName(u) == AuthStore::CanonicalName(user_))
            {
              return DbStatus::Error(DbCode::kLastAdmin, "不能撤销自己的管理员权限: " + u);
            }
            st = auth.SetAdmin(u, false, note);
          }
        }
        else if (cmd.kind == Kind::kGrant)
        {
          st = auth.GrantPrivilege(u, db, tbl, priv, false, note);
        }
        else
        {
          st = auth.RevokePrivilege(u, db, tbl, priv, note);
        }
        if (!st.ok())
        {
          return st;
        }
      }
    }
    return DbStatus::Ok();
  }

  DbStatus Session::RequireAdmin(const char *what) const
  {
    if (!engine_->config().enable_auth || is_admin())
    {
      return DbStatus::Ok();
    }
    return DbStatus::Error(DbCode::kPermissionDenied,
                           std::string("权限不足：") + what + " 需要管理员（当前用户 \"" + user_ +
                               "\"）");
  }

  DbStatus Session::CheckDatabaseAccess(const std::string &db) const
  {
    if (!engine_->config().enable_auth || is_admin())
    {
      return DbStatus::Ok();
    }
    if (!engine_->auth().HasAnyPrivilegeOnDb(user_, db))
    {
      return DbStatus::Error(DbCode::kPermissionDenied,
                             "权限不足：用户 \"" + user_ + "\" 不能访问数据库 " + db);
    }
    return DbStatus::Ok();
  }

  DbStatus Session::CheckDbPrivilege(cella::db::Priv need, const char *what) const
  {
    if (!engine_->config().enable_auth || is_admin())
    {
      return DbStatus::Ok();
    }
    const std::string &db = engine_->current_db();
    if (engine_->auth().HasPrivilege(user_, db, "*", need))
    {
      return DbStatus::Ok();
    }
    return DbStatus::Error(DbCode::kPermissionDenied,
                           "权限不足：用户 \"" + user_ + "\" 在库 " + db + " 上没有 " +
                               PrivName(need) + " 权限（" + what + "）");
  }

  void Session::FilterDatabasesByPrivilege(QueryResult *result) const
  {
    if (result == nullptr || !engine_->config().enable_auth || is_admin())
    {
      return;
    }
    std::vector<std::vector<storage::Value>> kept;
    for (const auto &row : result->rows)
    {
      if (row.empty())
      {
        continue;
      }
      if (engine_->auth().HasAnyPrivilegeOnDb(user_, row[0].str_val))
      {
        kept.push_back(row);
      }
    }
    result->rows = std::move(kept);
    result->tag = "SHOW DATABASES " + std::to_string(result->rows.size());
  }

  DbStatus Session::ExecuteOne(const std::string &stmt_text, int line, int col, StatementOutcome *out)
  {
    if (out == nullptr)
    {
      return DbStatus::Error(DbCode::kInternal, "ExecuteOne: out 为空");
    }
    const auto t0 = Clock::now();
    out->sql = stmt_text;
    out->line = line;
    out->col = col;

    // ── ⓪ 登录检查：认证已启用时必须先登录 ──
    if (need_login())
    {
      out->status = DbStatus::Error(DbCode::kNoCredentials,
                                    "尚未登录：请提供用户名与口令后再执行语句");
      out->elapsed_ms = MsSince(t0);
      return out->status;
    }

    // ── ① 事务控制语句（编译器不认识 BEGIN/COMMIT/ROLLBACK，在此拦截）──
    std::string keyword;
    if (IsTxnControl(stmt_text, &keyword))
    {
      std::string note;
      DbStatus st;
      if (keyword == "BEGIN")
      {
        st = Begin(&note);
        if (st.ok())
        {
          out->txn_id = txn_;
        }
      }
      else if (keyword == "COMMIT")
      {
        st = Commit(&note);
        if (st.ok() && engine_->config().checkpoint_on_commit)
        {
          (void)engine_->Checkpoint(); // 提交即落盘（可选，默认关）
        }
      }
      else
      {
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
    if (IsDatabaseControl(stmt_text, &db_kind, &db_arg))
    {
      out->kind = db_kind;
      DbStatus st;
      std::string note;
      // SHOW DATABASES / SHOW INDEXES 是只读的，事务中放行；CREATE/DROP/USE
      // 会换库或动文件系统，事务中一律拒绝（undo 记的是旧库表名，跨库切换会悬空）。
      const bool readonly_show = (db_kind == "SHOW DATABASES" || db_kind == "SHOW INDEXES" ||
                                  db_kind == "EXPLAIN");
      if (!readonly_show && in_transaction())
      {
        st = DbStatus::Error(DbCode::kDatabaseTxnActive,
                             db_kind + " 前须结束当前事务（COMMIT / ROLLBACK）");
      }
      else if ((db_kind == "CREATE DATABASE" || db_kind == "DROP DATABASE") && !is_admin())
      {
        // 建库/删库是全局操作，只有管理员可以做
        st = DbStatus::Error(DbCode::kPermissionDenied, db_kind + " 需要管理员权限");
      }
      else if (db_kind == "CREATE DATABASE")
      {
        st = engine_->CreateDatabase(db_arg, &note);
      }
      else if (db_kind == "DROP DATABASE")
      {
        st = engine_->DropDatabase(db_arg, &note);
      }
      else if (db_kind == "USE")
      {
        if (db_arg.empty())
        {
          st = DbStatus::Error(DbCode::kDatabaseError, "USE 缺少数据库名");
        }
        else
        {
          // 访问控制：在该库上没有任何授权就不能切过去
          st = CheckDatabaseAccess(db_arg);
          if (st.ok())
          {
            st = engine_->UseDatabase(db_arg, &note);
          }
        }
      }
      else if (db_kind == "SHOW INDEXES")
      { // SHOW INDEXES [IN table]
        st = engine_->ShowIndexes(db_arg, &out->result);
        note = out->result.tag;
      }
      else if (db_kind == "EXPLAIN")
      { // EXPLAIN <语句>：只编译 + 选访问路径，不取数据
        st = engine_->Explain(db_arg, &out->result);
        note = out->result.tag;
      }
      else
      { // SHOW DATABASES
        st = engine_->ShowDatabases(&out->result);
        FilterDatabasesByPrivilege(&out->result); // 普通用户只看得到有权限的库
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
      if (pres == AuthParse::kSyntaxError)
      {
        out->kind = "USER";
        out->status = DbStatus::Error(DbCode::kSqlError, "语法错误: " + perr);
        out->elapsed_ms = MsSince(t0);
        return out->status;
      }
      if (pres == AuthParse::kOk)
      {
        std::string note;
        const DbStatus st = ApplyUserCommand(cmd, &note, &out->result);
        out->kind = "USER";
        out->executed = true;
        out->status = st;
        if (st.ok() && out->result.tag.empty())
        {
          out->result.tag = note;
        }
        out->elapsed_ms = MsSince(t0);
        DbLogInfo(logcat::kSession,
                  "[" + name_ + "] USER " + (st.ok() ? "OK: " + note : st.ToString()));
        return st;
      }
      // kNotAuth → 落到编译器（普通 SQL）
    }

    // ── ①d 授权语句（GRANT / REVOKE / SHOW GRANTS，同样是会话层拦截）──
    {
      GrantCommand cmd;
      std::string perr;
      const AuthParse pres = ParseGrantCommand(stmt_text, &cmd, &perr);
      if (pres == AuthParse::kSyntaxError)
      {
        out->kind = "GRANT";
        out->status = DbStatus::Error(DbCode::kSqlError, "语法错误: " + perr);
        out->elapsed_ms = MsSince(t0);
        return out->status;
      }
      if (pres == AuthParse::kOk)
      {
        std::string note;
        const DbStatus st = ApplyGrantCommand(cmd, &note, &out->result);
        out->kind = (cmd.kind == GrantCommand::Kind::kShowGrants)
                        ? "SHOW GRANTS"
                        : ((cmd.kind == GrantCommand::Kind::kGrant) ? "GRANT" : "REVOKE");
        out->executed = true;
        out->status = st;
        if (st.ok() && out->result.tag.empty())
        {
          out->result.tag = note;
        }
        out->elapsed_ms = MsSince(t0);
        DbLogInfo(logcat::kSession,
                  "[" + name_ + "] " + out->kind + " " + (st.ok() ? "OK: " + note : st.ToString()));
        return st;
      }
    }

    // ── ② 编译：词法 → 语法 → 语义 → 计划 ──
    std::vector<cella::CELLA_Error> errors;
    const auto tokens = cella::cella_tokenize(stmt_text, errors);
    auto program = cella::cella_parse(tokens, errors);

    if (!program || !errors.empty())
    {
      out->compile_errors = errors;
      out->status = DbStatus::Error(DbCode::kSqlError, CompileErrorsText(errors));
      EnsureCatalogInSync(); // 语义阶段可能已改动目录副本 → 复位
      out->elapsed_ms = MsSince(t0);
      return out->status;
    }
    if (program->statements.empty())
    {
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

    if (!errors.empty() || plans.empty())
    {
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

    // ── 访问控制：DDL 的库级权限（建表 / 删表）──
    // 表级 DML 由执行器在 LockTable 这一必经点判定；这里管需要「库级」权限的两条。
    {
      const cella::CELLA_Stmt::Kind sk = program->statements[0]->kind;
      DbStatus ps;
      if (sk == cella::CELLA_Stmt::Kind::CREATE_TABLE)
      {
        ps = CheckDbPrivilege(Priv::kCreate, "CREATE TABLE");
      }
      else if (sk == cella::CELLA_Stmt::Kind::DROP_TABLE)
      {
        ps = CheckDbPrivilege(Priv::kDrop, "DROP TABLE");
      }
      else if (sk == cella::CELLA_Stmt::Kind::CREATE_INDEX)
      {
        // 建索引会改写表的物理布局语义 → 与建表同级，要求 kCreate
        ps = CheckDbPrivilege(Priv::kCreate, "CREATE INDEX");
      }
      else if (sk == cella::CELLA_Stmt::Kind::DROP_INDEX)
      {
        ps = CheckDbPrivilege(Priv::kDrop, "DROP INDEX");
      }
      if (!ps.ok())
      {
        // 语义阶段可能已把这张（不存在的）表登记进编译器目录副本 → 必须复位，
        // 否则下一次同名建表会误报「表已存在」（本块是提前返回，不会走到后面的复位）。
        EnsureCatalogInSync();
        out->status = ps;
        out->elapsed_ms = MsSince(t0);
        return ps;
      }
    }

    // ── ③ 计划优化：常量折叠 / 布尔化简 / 恒真 Filter 消除 ──
    // 执行用的是优化后的计划 → 「编译器产出的可执行 IR 直接驱动存储访问」
    const auto opt = cella::cella_optimizePlans(plans);
    out->plan_text = PlanText(opt.plans);
    if (opt.plans.empty())
    {
      out->status = DbStatus::Error(DbCode::kInternal, "优化后计划为空");
      out->elapsed_ms = MsSince(t0);
      return out->status;
    }

    // ── ④ 事务上下文：显式事务优先，否则为单语句开自动提交事务 ──
    // DDL 例外：建表/删表/建删索引会立即改写目录（元数据无法回滚），故按 MySQL 惯例
    // 先隐式提交前置事务，再以自动提交方式执行 DDL，避免出现「目录已改、事务回滚」
    // 造成的元数据与数据不一致。
    const bool is_ddl = (out->kind == "CREATE TABLE" || out->kind == "DROP TABLE" ||
                         out->kind == "CREATE INDEX" || out->kind == "DROP INDEX");
    if (is_ddl && in_transaction())
    {
      const txn_id_t prev = txn_;
      std::string note;
      const DbStatus cs = Commit(&note);
      out->implicit_commit = true;
      out->notice = "DDL 隐式提交前置事务 txn=" + std::to_string(prev) +
                    (cs.ok() ? std::string() : std::string("（提交失败）"));
      DbLogInfo(logcat::kSession, out->notice);
    }

    const bool own_txn = !in_transaction();
    if (own_txn)
    {
      (void)BeginInternal();
    }
    out->auto_committed = own_txn; // 本条语句是否跑在自建的自动提交事务里
    out->txn_id = txn_;
    const size_t undo_mark = engine_->txn_manager().UndoMark(txn_);
    if (txn_handle_)
    {
      txn_handle_->AddStatement();
    }

    ExecContext ctx;
    ctx.txn_id = txn_;
    ctx.txn = txn_handle_.get();
    ctx.lock_scope = engine_->current_db();
    // 访问控制：把身份带进执行上下文 —— 表级读写由 LockTable 判定
    ctx.auth_enabled = engine_->config().enable_auth;
    ctx.is_admin = is_admin();
    ctx.user = user_;
    // rowid 伪列：只在该语句确实引用它时才让扫描在结果末尾附加（否则 `get *` 会多出一列）
    ctx.with_rowid =
        program->statements.empty() ? false : StmtRefersRowid(program->statements[0].get());

    Executor &executor = *engine_->executor_;
    executor.ResetOperatorCalls();
    QueryResult result;
    const DbStatus st = executor.Execute(*opt.plans[0], ctx, &result);
    out->operator_calls = executor.operator_calls();

    // ── ⑤ 收尾：提交 / 语句级回滚 / 整事务回滚 ──
    if (st.ok())
    {
      out->executed = true;
      out->result = result;
      if (own_txn)
      {
        std::string note;
        const DbStatus cs = Commit(&note);
        out->auto_committed = true;
        if (!cs.ok())
        {
          out->status = cs;
          out->elapsed_ms = MsSince(t0);
          return cs;
        }
        // 自动提交 = 一次真正的事务提交。DDL 必须立刻存盘：目录行和物理表目录
        // 都只是缓冲池里的脏页，进程被强杀会一起丢，下次打开目录里就没这张表。
        // 其余语句按 checkpoint_on_commit 决定。
        if (is_ddl || engine_->config().checkpoint_on_commit)
        {
          const DbStatus cp = engine_->Checkpoint();
          if (!cp.ok() && is_ddl)
          {
            DbLogWarn(logcat::kEngine, "DDL 后存盘点失败: " + cp.message());
          }
        }
      }
    }
    else
    {
      out->status = st;
      const bool lock_victim = (st.code() == DbCode::kDeadlock || st.code() == DbCode::kLockConflict);
      if (own_txn || lock_victim)
      {
        // 自动提交失败 → 整个事务回滚；死锁/锁超时牺牲者 → 必须整事务回滚
        std::string note;
        (void)Rollback(&note);
      }
      else
      {
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

} // namespace cella::db
