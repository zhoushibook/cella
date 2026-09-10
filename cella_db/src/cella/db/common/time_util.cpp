#include "cella/db/common/time_util.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace cella::db {
namespace {

std::tm LocalTm(std::time_t t) {
  std::tm out{};
#if defined(_WIN32)
  localtime_s(&out, &t);
#else
  localtime_r(&t, &out);
#endif
  return out;
}

}  // namespace

std::string NowDateTime() {
  const std::time_t t = std::time(nullptr);
  const std::tm tm = LocalTm(t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

std::string NowIso() {
  const std::time_t t = std::time(nullptr);
  const std::tm tm = LocalTm(t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

int64_t NowEpochSeconds() { return static_cast<int64_t>(std::time(nullptr)); }

std::string FormatMillis(double ms) {
  std::ostringstream os;
  if (ms < 1.0) {
    os.precision(3);
    os << std::fixed << ms << " ms";
  } else if (ms < 1000.0) {
    os.precision(2);
    os << std::fixed << ms << " ms";
  } else {
    os.precision(3);
    os << std::fixed << (ms / 1000.0) << " s";
  }
  return os.str();
}

}  // namespace cella::db
