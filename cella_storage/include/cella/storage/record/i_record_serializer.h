#pragma once
#include <cstddef>
#include <vector>
#include "cella/storage/common/record.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/status.h"

namespace cella::storage {

// ── 记录序列化抽象：新增格式 = 新文件实现本接口 ───────────────
class IRecordSerializer {
 public:
  virtual ~IRecordSerializer() = default;

  virtual Status Serialize(const Record& record, const Schema& schema,
                           std::vector<char>* out) = 0;
  virtual Status Deserialize(const char* data, size_t len, const Schema& schema,
                             Record* out) = 0;
};

}  // namespace cella::storage
