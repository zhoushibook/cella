// mini_test.h —— 极简测试框架（零依赖）。
//
// 特点：
//   * 自注册用例：MT_TEST(名字) 即可，无需集中登记；
//   * 双路输出：控制台 + 可选日志文件（每行 flush），避免重定向缓冲丢输出；
//   * 支持按用例名子串过滤（便于定位单条用例）。
//
// 用法：
//   MT_TEST(我的用例) { MT_CHECK(1 + 1 == 2); MT_EQ(2, 2); }
//   int main() { return mt::RunAll(filter, log_path); }
#pragma once

#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mt {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& Cases() {
  static std::vector<Case> cases;
  return cases;
}
inline int& FailureCount() {
  static int n = 0;
  return n;
}
inline int& CheckCount() {
  static int n = 0;
  return n;
}
inline std::ofstream* LogFile() {
  static std::ofstream f;
  return &f;
}
inline bool& LogOpen() {
  static bool b = false;
  return b;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    Cases().push_back(Case{std::move(name), std::move(fn)});
  }
};

// 同时写控制台与日志文件（逐行 flush，保证任何退出路径下日志都完整）
inline void Say(const std::string& line) {
  std::cout << line << "\n";
  std::cout.flush();
  if (LogOpen()) {
    *LogFile() << line << "\n";
    LogFile()->flush();
  }
}

inline void ReportFailure(const char* file, int line, const std::string& msg) {
  ++FailureCount();
  std::ostringstream os;
  os << "    [失败] " << file << ":" << line << "  " << msg;
  Say(os.str());
}

inline bool StartLog(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  LogFile()->open(path, std::ios::binary | std::ios::trunc);
  LogOpen() = LogFile()->is_open();
  return LogOpen();
}

inline int RunAll(const std::string& filter, const std::string& log_path) {
  std::cout.setf(std::ios::unitbuf);
  if (!log_path.empty()) {
    StartLog(log_path);
  }

  int passed = 0;
  int failed = 0;
  int skipped = 0;
  for (auto& c : Cases()) {
    if (!filter.empty() && c.name.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    const int before = FailureCount();
    Say("[运行] " + c.name);
    try {
      c.fn();
    } catch (const std::exception& e) {
      ReportFailure(__FILE__, __LINE__, std::string("抛出异常: ") + e.what());
    } catch (...) {
      ReportFailure(__FILE__, __LINE__, "抛出未知异常");
    }
    if (FailureCount() == before) {
      ++passed;
      Say("[通过] " + c.name);
    } else {
      ++failed;
      Say("[失败] " + c.name);
    }
  }

  std::ostringstream os;
  os << "\n========== 通过 " << passed << " / 失败 " << failed;
  if (skipped != 0) {
    os << "（过滤跳过 " << skipped << "）";
  }
  os << "；断言 " << CheckCount() << " 次，失败 " << FailureCount() << " 次 ==========";
  Say(os.str());
  return failed == 0 ? 0 : 1;
}

}  // namespace mt

#define MT_TEST(name)                              \
  static void mt_case_##name();                    \
  static ::mt::Registrar mt_reg_##name(#name, mt_case_##name); \
  static void mt_case_##name()

#define MT_CHECK(cond)                                                     \
  do {                                                                     \
    ++::mt::CheckCount();                                                  \
    if (!(cond)) {                                                         \
      ::mt::ReportFailure(__FILE__, __LINE__, "断言不成立: " #cond);        \
    }                                                                      \
  } while (0)

#define MT_EQ(actual, expected)                                                                \
  do {                                                                                         \
    ++::mt::CheckCount();                                                                      \
    const auto& mt_a = (actual);                                                               \
    const auto& mt_b = (expected);                                                             \
    if (!(mt_a == mt_b)) {                                                                     \
      std::ostringstream mt_os;                                                                \
      mt_os << "期望 [" << mt_b << "]，实际 [" << mt_a << "]  (" #actual ")";                   \
      ::mt::ReportFailure(__FILE__, __LINE__, mt_os.str());                                     \
    }                                                                                          \
  } while (0)
