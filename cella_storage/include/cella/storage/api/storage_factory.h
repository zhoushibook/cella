#pragma once
#include <memory>

#include "i_storage.h"

namespace cella::storage {

// ── 工厂：按配置创建存储实现（当前为 FileStorage）────────────
std::unique_ptr<IStorage> CreateStorage(const StorageConfig& config);

}  // namespace cella::storage
