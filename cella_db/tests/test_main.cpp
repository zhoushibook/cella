// test_main.cpp —— cella_db 测试入口。
//
// 用法：
//   cella_db_tests [用例名子串] [--log 日志文件路径]
// 例：
//   cella_db_tests                        # 跑全部用例
//   cella_db_tests 事务                    # 只跑名字含「事务」的用例
//   cella_db_tests --log ../test_report.log
#include <iostream>
#include <string>

#include "mini_test.h"

int main(int argc, char** argv) {
  std::string filter;
  std::string log_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--log" && i + 1 < argc) {
      log_path = argv[++i];
    } else if (a.rfind("--log=", 0) == 0) {
      log_path = a.substr(6);
    } else if (filter.empty()) {
      filter = a;
    }
  }
  {
    std::string head = "===== cella_db 测试";
    if (!filter.empty()) {
      head += "（过滤: " + filter + "）";
    }
    head += " =====";
    mt::Say(head);
  }
  return mt::RunAll(filter, log_path);
}
