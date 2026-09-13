// db_logger.h —— 整合层日志门面。
//
// 复用存储层的 ILogger 抽象（ConsoleLogger / FileLogger），不重复造后端；
// 本层只补两件事：
//   1) 级别过滤（避免 Debug 日志污染正常输出）；
//   2) 分类（category）约定——catalog / exec / txn / lock / engine / session，
//      便于按子系统检索（配合 FileLogger 的 [CATEGORY] 字段）。
#pragma once

#include <string>

#include "cella/storage/common/logger.h"

namespace cella::db {

// 日志分类常量（与文档 §日志 一致）
namespace logcat {
constexpr const char* kEngine  = "engine";
constexpr const char* kCatalog = "catalog";
constexpr const char* kExec    = "exec";
constexpr const char* kTxn     = "txn";
constexpr const char* kLock    = "lock";
constexpr const char* kSession = "session";
constexpr const char* kAuth    = "auth";
}  // namespace logcat

// ── 日志门面：可多实例（各组件各持一份），另提供进程级 Global() ──
class DbLogger {
 public:
  // 绑定后端（不接管所有权；传 nullptr 表示丢弃日志）
  void Attach(storage::ILogger* backend) { backend_ = backend; }
  void SetLevel(storage::LogLevel level) { level_ = level; }
  storage::LogLevel level() const { return level_; }
  bool enabled(storage::LogLevel level) const {
    return backend_ != nullptr && static_cast<int>(level) >= static_cast<int>(level_);
  }

  // 按级别输出一条分类日志
  void Write(storage::LogLevel level, const std::string& category, const std::string& message) {
    if (!enabled(level)) {
      return;
    }
    backend_->Log(level, category, message);
  }

  void Debug(const std::string& c, const std::string& m) { Write(storage::LogLevel::kDebug, c, m); }
  void Info(const std::string& c, const std::string& m) { Write(storage::LogLevel::kInfo, c, m); }
  void Warn(const std::string& c, const std::string& m) { Write(storage::LogLevel::kWarn, c, m); }
  void Error(const std::string& c, const std::string& m) { Write(storage::LogLevel::kError, c, m); }

  // 进程级默认实例：CLI / 门面在 Open 时 Attach 到 FileLogger 或 ConsoleLogger
  static DbLogger& Global();

 private:
  storage::ILogger* backend_ = nullptr;
  storage::LogLevel level_ = storage::LogLevel::kInfo;
};

// ── 便捷函数（走 Global()）───────────────────────────────────
inline void DbLogDebug(const std::string& c, const std::string& m) { DbLogger::Global().Debug(c, m); }
inline void DbLogInfo(const std::string& c, const std::string& m) { DbLogger::Global().Info(c, m); }
inline void DbLogWarn(const std::string& c, const std::string& m) { DbLogger::Global().Warn(c, m); }
inline void DbLogError(const std::string& c, const std::string& m) { DbLogger::Global().Error(c, m); }

// ── 双写日志：同时投递到两个后端（如「文件 + 控制台」）────────
// 不持有后端所有权；后端生命周期由调用方（DbEngine）保证。
class TeeLogger : public storage::ILogger {
 public:
  TeeLogger(storage::ILogger* first, storage::ILogger* second) : first_(first), second_(second) {}

  void Log(storage::LogLevel level, const std::string& category,
           const std::string& message) override {
    if (first_ != nullptr) {
      first_->Log(level, category, message);
    }
    if (second_ != nullptr) {
      second_->Log(level, category, message);
    }
  }
  void Flush() override {
    if (first_ != nullptr) {
      first_->Flush();
    }
    if (second_ != nullptr) {
      second_->Flush();
    }
  }

 private:
  storage::ILogger* first_ = nullptr;
  storage::ILogger* second_ = nullptr;
};

}  // namespace cella::db
