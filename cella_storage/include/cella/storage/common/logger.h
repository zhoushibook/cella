#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace cella::storage
{

  // ── 日志级别 ────────────────────────────────────────────────
  enum class LogLevel : uint8_t
  {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3
  };
  const char *ToString(LogLevel l);

  // ── 全局最低输出级别（默认 kInfo，DEBUG 默认不出）──────────────
  // 为什么需要：明细级日志（典型是缓冲池**每次换页**一条）在正常运行时没有
  // 保留价值，但历史上它们按 WARN 落盘 —— 一次恢复卡死就往存储日志里灌了 236MB。
  // 现在明细降到 DEBUG，默认被这里挡掉；排查时设环境变量 CELLA_LOG_LEVEL=debug
  // 即可全部放行。过滤放在 Console/File 两个内置后端里，不影响测试自定义的
  // ILogger 实现（它们直接收到全部调用）。
  LogLevel LogMinLevel();
  void SetLogMinLevel(LogLevel level);
  bool LogEnabled(LogLevel level);

  // ── 日志抽象：Console/File 两种实现；新增后端 = 新文件实现本接口 ──
  // 输出格式：[YYYY-MM-DD HH:MM:SS.mmm][LEVEL][CATEGORY] message
  class ILogger
  {
  public:
    virtual ~ILogger() = default;
    virtual void Log(LogLevel level, const std::string &category,
                     const std::string &message) = 0;
    virtual void Flush() {}
  };

  class ConsoleLogger : public ILogger
  {
  public:
    void Log(LogLevel level, const std::string &category,
             const std::string &message) override;
  };

  class FileLogger : public ILogger
  {
  public:
    explicit FileLogger(const std::string &path);
    ~FileLogger() override;
    FileLogger(const FileLogger &) = delete;
    FileLogger &operator=(const FileLogger &) = delete;

    void Log(LogLevel level, const std::string &category,
             const std::string &message) override;
    void Flush() override;

  private:
    struct Impl; // pimpl：头文件不暴露 <fstream>
    std::unique_ptr<Impl> impl_;
  };

} // namespace cella::storage
