#include "cella/storage/common/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 日志：统一输出格式 [YYYY-MM-DD HH:MM:SS.mmm][LEVEL][CATEGORY] message。
// ILogger 是抽象，ConsoleLogger 打到终端，FileLogger 追加写文件。
// 这样缓冲池/存储层只管调 Log()，输出到哪里由配置决定（约束 #6 扩展点之一）。
// ─────────────────────────────────────────────────────────────────────────

const char* ToString(LogLevel l) {
  switch (l) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO";
    case LogLevel::kWarn:  return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "UNKNOWN";
}

namespace {

// 生成「YYYY-MM-DD HH:MM:SS.mmm」时间戳（含毫秒）。
// 跨平台处理：Windows 用 localtime_s，POSIX 用 localtime_r（都是线程安全版）。
std::string FormatTimestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;   // 取毫秒部分
  const std::time_t t = system_clock::to_time_t(now);

  std::tm tm{};
#if defined(_MSC_VER)
  localtime_s(&tm, &t);     // MSVC 签名：(struct tm*, const time_t*)
#else
  localtime_r(&t, &tm);     // POSIX
#endif

  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
     << std::setw(3) << std::setfill('0') << ms.count();   // 毫秒补 0 到 3 位
  return os.str();
}

// 组装一整行日志：[时间][级别][类别] 消息
std::string FormatLine(LogLevel level, const std::string& category,
                       const std::string& message) {
  std::ostringstream os;
  os << '[' << FormatTimestamp() << "][" << ToString(level) << "]["
     << category << "] " << message;
  return os.str();
}

}  // namespace

void ConsoleLogger::Log(LogLevel level, const std::string& category,
                        const std::string& message) {
  const std::string line = FormatLine(level, category, message) + "\n";
  // 惯例：WARN/ERROR 走 stderr，DEBUG/INFO 走 stdout
  if (level >= LogLevel::kWarn) {
    std::fputs(line.c_str(), stderr);
  } else {
    std::fputs(line.c_str(), stdout);
  }
  std::fflush(nullptr);   // 立即刷出（否则管道缓冲下可能看不到）
}

// FileLogger 用 pimpl（指向实现）隐藏 <fstream> 细节，避免头文件引入 <fstream>，
// 同时用 unique_ptr 自动管理文件句柄（约束 #6 禁裸 new/delete）。
struct FileLogger::Impl {
  std::ofstream file;
};

FileLogger::FileLogger(const std::string& path) : impl_(std::make_unique<Impl>()) {
  impl_->file.open(path, std::ios::out | std::ios::app);   // 追加模式
}

FileLogger::~FileLogger() = default;   // 定义在这里（Impl 完整处），unique_ptr 才能析构

void FileLogger::Log(LogLevel level, const std::string& category,
                     const std::string& message) {
  if (impl_ && impl_->file.is_open()) {
    impl_->file << FormatLine(level, category, message) << '\n';
  }
}

void FileLogger::Flush() {
  if (impl_ && impl_->file.is_open()) {
    impl_->file.flush();
  }
}

}  // namespace cella::storage
