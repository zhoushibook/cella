// server_config.h —— cella_web 启动参数。
#pragma once

#include <cstdint>
#include <string>

namespace cella::client {

struct ServerConfig {
  std::string data_dir = "./cella_data";  // 数据目录（与 cella_db CLI 同义）
  std::string db_file = "main.db";        // 启动库
  std::uint16_t port = 8080;              // 只绑 127.0.0.1
  std::string web_dir;                    // web/ 资源目录；空 = 编译期默认
  std::uint32_t page_size = 4096;
  size_t pool_size = 64;
  std::string replacer = "LRU";
  bool help = false;
  std::string error;                      // 解析失败原因
};

// 解析命令行；失败时 config.error 给出原因
ServerConfig ParseServerArgs(int argc, char** argv);

}  // namespace cella::client
