#include "mini_test.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// 文档示例验证（§12 最高优先级）：编译由 CMake 保证，这里运行并核对输出。
// QUICKSTART_PATH / CRUD_FLOW_PATH 由 CMake 注入（正向斜杠绝对路径）。

namespace {

bool RunExample(const std::string& exe, const std::string& marker) {
  const std::string out = "_example_out.txt";
  const std::string cmd = "\"" + exe + "\" > " + out + " 2>&1";
  const int rc = std::system(cmd.c_str());
  std::ifstream f(out);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();
  std::remove(out.c_str());
  return rc == 0 && content.find(marker) != std::string::npos;
}

}  // namespace

TEST_CASE(example_quickstart) {
  std::filesystem::remove_all("./quickstart_data");
  EXPECT_TRUE(RunExample(QUICKSTART_PATH, "quickstart OK"));
  std::filesystem::remove_all("./quickstart_data");
}

TEST_CASE(example_crud_flow) {
  std::filesystem::remove_all("./crud_data");
  EXPECT_TRUE(RunExample(CRUD_FLOW_PATH, "crud OK"));
  std::filesystem::remove_all("./crud_data");
}
