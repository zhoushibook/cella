// server_config.cpp —— 启动参数解析。
#include "cella/client/server/server_config.h"

#include <cstdlib>

namespace cella::client {

ServerConfig ParseServerArgs(int argc, char** argv) {
  ServerConfig c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string* dst) -> bool {
      if (i + 1 >= argc) {
        c.error = "选项缺少参数: " + a;
        return false;
      }
      *dst = argv[++i];
      return true;
    };
    if (a == "-h" || a == "--help") {
      c.help = true;
    } else if (a == "--data") {
      if (!value(&c.data_dir)) return c;
    } else if (a == "--db") {
      if (!value(&c.db_file)) return c;
    } else if (a == "--port") {
      std::string v;
      if (!value(&v)) return c;
      const int n = std::atoi(v.c_str());
      if (n <= 0 || n > 65535) {
        c.error = "端口非法: " + v;
        return c;
      }
      c.port = static_cast<std::uint16_t>(n);
    } else if (a == "--web-dir") {
      if (!value(&c.web_dir)) return c;
    } else if (a == "--page-size") {
      std::string v;
      if (!value(&v)) return c;
      c.page_size = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (a == "--pool") {
      std::string v;
      if (!value(&v)) return c;
      c.pool_size = static_cast<size_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (a == "--replacer") {
      if (!value(&c.replacer)) return c;
    } else {
      c.error = "未知选项: " + a;
      return c;
    }
  }
  return c;
}

}  // namespace cella::client
