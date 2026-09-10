#pragma once
#include "cella/storage/page/page.h"
#include "cella/storage/page/page_layout.h"

namespace cella::storage {

// ── 槽式布局实现（§8）───────────────────────────────────────
// PageHeader(32B) | 槽数组(向后增长，每项4B{offset:u16,len:u16}) | 空闲 | 记录数据(向前)
// len==0 表示已删除（墓碑）。剩余空间 = free_end - (32 + slot_count*4)
class SlottedPageLayout : public PageLayout {
 public:
  explicit SlottedPageLayout(Page* page) : page_(page) {}

  Status InsertRecord(const char* record, uint16_t len, slot_id_t* out) override;
  Status GetRecord(slot_id_t slot, const char** data, uint16_t* len) override;
  Status DeleteRecord(slot_id_t slot) override;
  uint16_t GetFreeSpace() const override;

 private:
  Page* page_;
};

}  // namespace cella::storage
