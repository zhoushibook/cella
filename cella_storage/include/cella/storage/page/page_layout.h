#pragma once
#include "cella/storage/common/status.h"
#include "cella/storage/common/types.h"

namespace cella::storage {

// ── 页布局抽象：新增布局 = 新文件实现本接口 ──────────────────
class PageLayout {
 public:
  virtual ~PageLayout() = default;

  virtual Status InsertRecord(const char* record, uint16_t len, slot_id_t* out) = 0;
  virtual Status GetRecord(slot_id_t slot, const char** data, uint16_t* len) = 0;
  virtual Status DeleteRecord(slot_id_t slot) = 0;   // 标记删除（槽 len 置 0）
  virtual uint16_t GetFreeSpace() const = 0;
};

}  // namespace cella::storage
