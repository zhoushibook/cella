// test_main.cpp —— 客户端层测试入口。
#include <string>

#include "mini_test.h"

int main(int argc, char** argv) {
  std::string filter;
  std::string log = "client_test_report.log";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (a == "--log" && i + 1 < argc) {
      log = argv[++i];
    } else if (a == "--no-log") {
      log.clear();
    }
  }
  return mt::RunAll(filter, log);
}
