#include "cella/storage/common/logger.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace cella::storage
{

  // ─────────────────────────────────────────────────────────────────────────
  // 日志：统一输出格式 [YYYY-MM-DD HH:MM:SS.mmm][LEVEL][CATEGORY] message。
  // ILogger 是抽象，ConsoleLogger 打到终端，FileLogger 追加写文件。
  // 这样缓冲池/存储层只管调 Log()，输出到哪里由配置决定（约束 #6 扩展点之一）。
  // ─────────────────────────────────────────────────────────────────────────

  const char *ToString(LogLevel l)
  {
    switch (l)
    {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
    }
    return "UNKNOWN";
  }

  namespace
  {

    // 读环境变量（不存在 → 空串）。
    // MSVC 把裸 getenv 视为不安全（C4996，本项目 /WX 下直接算错误），Windows 走
    // _dupenv_s；其它平台保持 getenv，行为一致。
    std::string ReadEnv(const char *name)
    {
#if defined(_MSC_VER)
      char *buf = nullptr;
      size_t len = 0;
      if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr)
      {
        return std::string();
      }
      std::string value(buf);
      std::free(buf);
      return value;
#else
      const char *env = std::getenv(name);
      return env == nullptr ? std::string() : std::string(env);
#endif
    }

    // 「CELLA_LOG_LEVEL=debug|info|warn|error」→ 级别；缺省 / 无法识别 → INFO
    LogLevel ParseLevelName(std::string name)
    {
      for (char &c : name)
      {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (name == "debug")
        return LogLevel::kDebug;
      if (name == "warn" || name == "warning")
        return LogLevel::kWarn;
      if (name == "error")
        return LogLevel::kError;
      return LogLevel::kInfo;
    }

    // 函数内 static：环境变量只在首次使用时读一次；atomic 让运行期改级别无数据竞争。
    std::atomic<LogLevel> &MinLevelSlot()
    {
      static std::atomic<LogLevel> level = []
      {
        const std::string env = ReadEnv("CELLA_LOG_LEVEL");
        return env.empty() ? LogLevel::kInfo : ParseLevelName(env);
      }();
      return level;
    }

  } // namespace

  LogLevel LogMinLevel() { return MinLevelSlot().load(std::memory_order_relaxed); }

  void SetLogMinLevel(LogLevel level)
  {
    MinLevelSlot().store(level, std::memory_order_relaxed);
  }

  bool LogEnabled(LogLevel level)
  {
    return static_cast<uint8_t>(level) >= static_cast<uint8_t>(LogMinLevel());
  }

  namespace
  {

    // 生成「YYYY-MM-DD HH:MM:SS.mmm」时间戳（含毫秒）。
    // 跨平台处理：Windows 用 localtime_s，POSIX 用 localtime_r（都是线程安全版）。
    std::string FormatTimestamp()
    {
      using namespace std::chrono;
      const auto now = system_clock::now();
      const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000; // 取毫秒部分
      const std::time_t t = system_clock::to_time_t(now);

      std::tm tm{};
#if defined(_MSC_VER)
      localtime_s(&tm, &t); // MSVC 签名：(struct tm*, const time_t*)
#else
      localtime_r(&t, &tm); // POSIX
#endif

      std::ostringstream os;
      os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << ms.count(); // 毫秒补 0 到 3 位
      return os.str();
    }

    // 组装一整行日志：[时间][级别][类别] 消息
    std::string FormatLine(LogLevel level, const std::string &category,
                           const std::string &message)
    {
      std::ostringstream os;
      os << '[' << FormatTimestamp() << "][" << ToString(level) << "]["
         << category << "] " << message;
      return os.str();
    }

  } // namespace

  void ConsoleLogger::Log(LogLevel level, const std::string &category,
                          const std::string &message)
  {
    if (!LogEnabled(level))
    {
      return;
    }
    const std::string line = FormatLine(level, category, message) + "\n";
    // 惯例：WARN/ERROR 走 stderr，DEBUG/INFO 走 stdout
    if (level >= LogLevel::kWarn)
    {
      std::fputs(line.c_str(), stderr);
    }
    else
    {
      std::fputs(line.c_str(), stdout);
    }
    std::fflush(nullptr); // 立即刷出（否则管道缓冲下可能看不到）
  }

  // FileLogger 用 pimpl（指向实现）隐藏 <fstream> 细节，避免头文件引入 <fstream>，
  // 同时用 unique_ptr 自动管理文件句柄（约束 #6 禁裸 new/delete）。
  //
  // 文件大小上限 + 轮转：日志是**追加**写的，只增不减 —— 一次异常（比如恢复
  // 卡死时每页一条明细）就能把文件撑到几百 MB。这里按上限轮转：超限时当前文件
  // 改名 <path>.1（覆盖上一份备份，只留一份），并新开一个空文件继续写。
  // 上限默认 32MB，可用 CELLA_LOG_MAX_MB 覆盖（单位 MB；0 = 关闭轮转）。
  struct FileLogger::Impl
  {
    std::string path;
    std::ofstream file;
    uint64_t bytes_written = 0; // 当前文件的近似字节数（轮转判定用）
    uint64_t max_bytes = 0;     // 0 = 不轮转

    void RotateIfNeeded()
    {
      if (max_bytes == 0 || bytes_written < max_bytes)
      {
        return;
      }
      file.close();
      std::error_code ec;
      const std::string backup = path + ".1";
      std::filesystem::remove(backup, ec); // 旧备份直接丢弃（日志不是归档数据）
      std::filesystem::rename(path, backup, ec);
      file.open(path, std::ios::out | std::ios::app);
      if (!file.is_open())
      {
        return;
      }
      // 留一行交接说明，避免日志看起来「凭空断开」
      const std::string line = FormatLine(LogLevel::kInfo, "LOGGER",
                                          "日志达到上限 " + std::to_string(max_bytes >> 20) + " MB，已轮转到 " + backup);
      file << line << '\n';
      bytes_written = line.size() + 1;
    }
  };

  FileLogger::FileLogger(const std::string &path) : impl_(std::make_unique<Impl>())
  {
    impl_->path = path;
    impl_->max_bytes = uint64_t{32} << 20; // 默认 32MB
    const std::string max_mb = ReadEnv("CELLA_LOG_MAX_MB");
    if (!max_mb.empty())
    {
      impl_->max_bytes = static_cast<uint64_t>(std::strtoull(max_mb.c_str(), nullptr, 10)) << 20;
    }
    impl_->file.open(path, std::ios::out | std::ios::app); // 追加模式
    if (impl_->file.is_open())
    {
      // 上一版本可能已经写出了远超上限的文件：打开时就先轮转掉，别等下次写满
      std::error_code ec;
      const auto size = std::filesystem::file_size(path, ec);
      impl_->bytes_written = ec ? 0 : static_cast<uint64_t>(size);
      impl_->RotateIfNeeded();
    }
  }

  FileLogger::~FileLogger() = default; // 定义在这里（Impl 完整处），unique_ptr 才能析构

  void FileLogger::Log(LogLevel level, const std::string &category,
                       const std::string &message)
  {
    if (impl_ && impl_->file.is_open() && LogEnabled(level))
    {
      const std::string line = FormatLine(level, category, message);
      impl_->file << line << '\n';
      impl_->bytes_written += line.size() + 1;
      impl_->RotateIfNeeded();
    }
  }

  void FileLogger::Flush()
  {
    if (impl_ && impl_->file.is_open())
    {
      impl_->file.flush();
    }
  }

} // namespace cella::storage
