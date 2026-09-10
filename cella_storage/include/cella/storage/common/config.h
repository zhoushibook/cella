#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "logger.h"
#include "status.h"

namespace cella::storage {

// ── 存储配置：全部可配置，禁止硬编码（约束 #2）───────────────
struct StorageConfig {
  uint32_t page_size = 4096;             // 页大小：2 的幂且 >= 512
  size_t   pool_size = 64;               // 缓冲池帧数
  std::string replacer = "LRU";          // 替换策略名（LRU/FIFO/CLOCK...）
  std::string data_dir = "./data";       // 数据目录
  std::string db_file = "cella.db";      // 数据库文件名
  bool enable_log = true;                // 是否开启日志
  std::string log_path = "./data/cella-storage.log";
  bool enable_checksum = false;          // 数据页 CRC32（默认关）
  LogLevel log_level = LogLevel::kInfo;

  Status Validate() const;               // 配置合法性校验
  std::string ToString() const;          // 表格化配置摘要
};

}  // namespace cella::storage
