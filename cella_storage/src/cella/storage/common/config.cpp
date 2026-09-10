#include "cella/storage/common/config.h"

#include <sstream>

namespace cella::storage {

Status StorageConfig::Validate() const {
  // page_size 必须是 2 的幂且 >= 512（约束 #9）
  if (page_size < 512 || (page_size & (page_size - 1)) != 0) {
    return Status::Error(StatusCode::kInvalidConfig,
                         "page_size 必须是 2 的幂且 >= 512");
  }
  // 槽式布局 free_end/offset 为 u16，单页上限 65535
  if (page_size > 65535) {
    return Status::Error(StatusCode::kInvalidConfig,
                         "page_size 必须 <= 65535（槽式布局偏移为 u16）");
  }
  if (pool_size == 0) {
    return Status::Error(StatusCode::kInvalidConfig, "pool_size 必须 > 0");
  }
  if (data_dir.empty() || db_file.empty()) {
    return Status::Error(StatusCode::kInvalidConfig, "data_dir/db_file 不能为空");
  }
  if (replacer.empty()) {
    return Status::Error(StatusCode::kInvalidConfig, "replacer 不能为空");
  }
  return Status::OK();
}

std::string StorageConfig::ToString() const {
  std::ostringstream os;
  os << "  page_size       : " << page_size << " bytes\n"
     << "  pool_size       : " << pool_size << " frames\n"
     << "  replacer        : " << replacer << "\n"
     << "  data_dir        : " << data_dir << "\n"
     << "  db_file         : " << db_file << "\n"
     << "  enable_log      : " << (enable_log ? "true" : "false") << "\n"
     << "  log_path        : " << log_path << "\n"
     << "  enable_checksum : " << (enable_checksum ? "true" : "false") << "\n"
     << "  log_level       : " << cella::storage::ToString(log_level);
  return os.str();
}

}  // namespace cella::storage
