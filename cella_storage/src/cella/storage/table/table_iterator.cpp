#include "cella/storage/table/table_iterator.h"

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/page/slotted_page_layout.h"
#include "cella/storage/record/i_record_serializer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// TableIterator：跨页扫描整张表，自动跳过「墓碑」（已标记删除的记录）。
//
// 定位规则：从首页第 0 槽开始，逐槽、逐页地找「下一条有效记录」。
// 有效 = 槽 len != 0 且能成功反序列化；墓碑（len==0）直接跳过。
// done_ == true 表示已经扫到头（等于 end 哨兵）。
// ─────────────────────────────────────────────────────────────────────────

TableIterator::TableIterator(BufferPoolManager* bpm, page_id_t first_page, const Schema& schema,
                             IRecordSerializer* serializer)
    : bpm_(bpm), page_id_(first_page), schema_(&schema), serializer_(serializer), done_(false) {
  advance_to_valid();                 // 构造时立刻定位到第一条有效记录（可能没有）
}

TableIterator& TableIterator::operator++() {
  ++slot_id_;                         // 从下一槽继续找
  advance_to_valid();
  return *this;
}

void TableIterator::advance_to_valid() {
  // 外层循环：逐页走（page_id_ 会沿 next 链前进）
  while (page_id_ != kInvalidPageId) {
    PageGuard pg(bpm_, bpm_->get_page(page_id_));
    if (!pg.valid()) {
      done_ = true;
      return;
    }
    SlottedPageLayout layout(pg.get());
    const uint16_t slot_count = pg->GetSlotCount();

    // 内层循环：在当前页里逐槽找有效记录
    while (slot_id_ < slot_count) {
      const char* data = nullptr;
      uint16_t len = 0;
      Status s = layout.GetRecord(slot_id_, &data, &len);
      if (s.ok()) {                   // len != 0，是有效记录（非墓碑）
        Status d = serializer_->Deserialize(data, len, *schema_, &current_);
        if (d.ok()) {
          current_rid_ = Rid{page_id_, slot_id_};
          done_ = false;
          return;                     // 找到了，停下
        }
      }
      ++slot_id_;                     // 墓碑或反序列化失败，看下一槽
    }

    // 本页扫完：跳到下一页，从第 0 槽重新开始
    const page_id_t next = pg->GetHeaderNextPageId();
    page_id_ = next;
    slot_id_ = 0;
  }
  done_ = true;                       // 所有页都扫完了
}

}  // namespace cella::storage
