#pragma once
#include <cstdint>

namespace cella::storage {

// 接口版本：出现「库版本与文档不一致」时能立刻定位（约束 #2.2 约定 3）
inline constexpr const char* kStorageVersion = "cella-storage/0.1";

// 数据文件格式版本：写入 MetaPage，用于识别旧版本数据文件
inline constexpr uint32_t kFormatVersion = 1;

}  // namespace cella::storage
