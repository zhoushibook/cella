#pragma once
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// ── 迷你测试框架（约束 #12）─────────────────────────────────
// 提供 TEST_CASE / EXPECT_EQ / EXPECT_TRUE / EXPECT_FALSE / EXPECT_OK。

namespace minitest {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& FailCount() {
  static int c = 0;
  return c;
}

inline int& CheckCount() {
  static int c = 0;
  return c;
}

inline void MarkCheck() { ++CheckCount(); }

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    Registry().push_back(TestCase{name, std::move(fn)});
  }
};

inline void ReportFailure(const char* file, int line, const std::string& expr,
                          const std::string& detail) {
  ++FailCount();
  std::fprintf(stderr, "  [FAIL] %s:%d  %s", file, line, expr.c_str());
  if (!detail.empty()) {
    std::fprintf(stderr, "  -> %s", detail.c_str());
  }
  std::fprintf(stderr, "\n");
}

inline int RunAll() {
  for (const auto& tc : Registry()) {
    std::printf("[ RUN  ] %s\n", tc.name);
    std::fflush(stdout);
    tc.fn();
  }
  std::printf("\n[=====] cases=%zu checks=%d failures=%d\n",
              Registry().size(), CheckCount(), FailCount());
  std::fflush(stdout);
  return FailCount() == 0 ? 0 : 1;
}

}  // namespace minitest

#define MINITEST_CONCAT_(a, b) a##b
#define MINITEST_CONCAT(a, b) MINITEST_CONCAT_(a, b)

#define TEST_CASE(name)                                           \
  static void MINITEST_CONCAT(test_fn_, name)();                  \
  static ::minitest::Registrar MINITEST_CONCAT(test_reg_, name)(  \
      #name, MINITEST_CONCAT(test_fn_, name));                    \
  static void MINITEST_CONCAT(test_fn_, name)()

#define EXPECT_TRUE(cond)                                          \
  do {                                                             \
    ::minitest::MarkCheck();                                       \
    if (!(cond)) {                                                 \
      ::minitest::ReportFailure(__FILE__, __LINE__, #cond, "");    \
    }                                                              \
  } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

// 仅适用于可流式输出（operator<<）的类型；enum class 等用 EXPECT_TRUE
#define EXPECT_EQ(a, b)                                                   \
  do {                                                                    \
    ::minitest::MarkCheck();                                              \
    const auto _minitest_va = (a);                                        \
    const auto _minitest_vb = (b);                                        \
    if (!(_minitest_va == _minitest_vb)) {                                \
      std::ostringstream _minitest_os;                                    \
      _minitest_os << "expect " #a " == " #b "  got " << _minitest_va     \
                   << " vs " << _minitest_vb;                             \
      ::minitest::ReportFailure(__FILE__, __LINE__, "EXPECT_EQ",          \
                                _minitest_os.str());                      \
    }                                                                     \
  } while (0)

#define EXPECT_OK(s)                                                     \
  do {                                                                   \
    ::minitest::MarkCheck();                                             \
    const auto _minitest_st = (s);                                       \
    if (!_minitest_st.ok()) {                                            \
      ::minitest::ReportFailure(__FILE__, __LINE__, "EXPECT_OK(" #s ")", \
                                _minitest_st.ToString());                \
    }                                                                    \
  } while (0)
