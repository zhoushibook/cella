#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace cella::storage {

// ── 日志级别 ────────────────────────────────────────────────
enum class LogLevel : uint8_t { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };
const char* ToString(LogLevel l);

// ── 日志抽象：Console/File 两种实现；新增后端 = 新文件实现本接口 ──
// 输出格式：[YYYY-MM-DD HH:MM:SS.mmm][LEVEL][CATEGORY] message
class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual void Log(LogLevel level, const std::string& category,
                   const std::string& message) = 0;
  virtual void Flush() {}
};

class ConsoleLogger : public ILogger {
 public:
  void Log(LogLevel level, const std::string& category,
           const std::string& message) override;
};

class FileLogger : public ILogger {
 public:
  explicit FileLogger(const std::string& path);
  ~FileLogger() override;
  FileLogger(const FileLogger&) = delete;
  FileLogger& operator=(const FileLogger&) = delete;

  void Log(LogLevel level, const std::string& category,
           const std::string& message) override;
  void Flush() override;

 private:
  struct Impl;                       // pimpl：头文件不暴露 <fstream>
  std::unique_ptr<Impl> impl_;
};

}  // namespace cella::storage
