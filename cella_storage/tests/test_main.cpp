#include "mini_test.h"

#include "console_utf8.h"   // 扁平名：demo/examples 只挂了扁平公共目录

int main() {
  cella::storage::EnableUtf8Console();   // 中文用例名/失败信息在控制台正常显示
  return ::minitest::RunAll();
}
