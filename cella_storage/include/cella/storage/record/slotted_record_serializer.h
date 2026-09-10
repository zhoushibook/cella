#pragma once
#include "cella/storage/record/i_record_serializer.h"

namespace cella::storage {

// ── 槽式变长记录序列化（§8）──────────────────────────────────
// 记录格式：| col_count:u16 | null_bitmap:ceil(n/8) | 列数据 |
// VARCHAR 前缀 len:u16；定长类型按小端写；NULL 由 bitmap 表示。
class SlottedRecordSerializer : public IRecordSerializer {
 public:
  Status Serialize(const Record& record, const Schema& schema,
                   std::vector<char>* out) override;
  Status Deserialize(const char* data, size_t len, const Schema& schema,
                     Record* out) override;
};

}  // namespace cella::storage
