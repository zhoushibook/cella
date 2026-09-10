#include "cella/storage/page/slotted_page_layout.h"

#include <cstring>

#include "cella/storage/common/byte_buffer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 槽式页布局（slotted page layout）—— 一页内如何摆放多条变长记录。
//
// 内存布局（一页 page_size 字节）：
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ PageHeader(32B) │ 槽数组 →向右增长 │     空闲区     │ 记录 →向左 │
//   └──────────────────────────────────────────────────────────────┘
//   低地址                                      free_end        高地址
//
//   - PageHeader(32B)：页元信息，含 slot_count（已有几条记录）和 free_end（空闲区起点）。
//   - 槽数组：每条记录占 4 字节 {offset:u16, len:u16}，从页头后面向右增长。
//     offset 指向该记录在页内的起始位置；len==0 表示该记录已删除（墓碑）。
//   - 记录数据：从页尾（高地址）向左增长，这样「槽数组」和「记录数据」相向增长，
//     中间的剩余空间可以完整利用。
//
//   剩余空间 = free_end - (32 + slot_count*4)
// ─────────────────────────────────────────────────────────────────────────

namespace {
constexpr size_t kSlotSize   = 4;    // 一个槽项 4 字节 {offset:u16, len:u16}
constexpr size_t kSlotOffset = 0;    // 槽项里 offset 字段的偏移
constexpr size_t kSlotLen    = 2;    // 槽项里 len 字段的偏移
}  // namespace

Status SlottedPageLayout::InsertRecord(const char* record, uint16_t len, slot_id_t* out) {
  uint16_t slot_count = page_->GetSlotCount();
  uint16_t free_end   = page_->GetFreeEnd();

  if (slot_count == 0) {
    // 空页：还没有记录，把 free_end 初始化为页大小（记录从页尾开始向左摆）
    free_end = static_cast<uint16_t>(page_->page_size());
    page_->SetFreeEnd(free_end);
  }

  // 检查① 记录是否过大：即使空页也放不下（至少需要 1 个槽项 + 记录本体）。
  // 单条记录上限 = page_size - 页头 - 1 个槽项。
  if (static_cast<size_t>(len) > page_->page_size() - kPageHeaderSize - kSlotSize) {
    return Status::Error(StatusCode::kRecordTooLarge, "记录超过单页容量上限");
  }

  // 检查② 页是否已满：算一下「槽数组末尾」和「记录区起点」之间还剩多少空间。
  //   slot_array_end = 页头 + 已有槽项占的字节
  //   data_space     = free_end - slot_array_end（当前可用空闲区）
  //   新记录要占  len（数据）+ kSlotSize（新增的一个槽项）
  const size_t slot_array_end = kPageHeaderSize + static_cast<size_t>(slot_count) * kSlotSize;
  const size_t data_space     = static_cast<size_t>(free_end) - slot_array_end;
  if (static_cast<size_t>(len) + kSlotSize > data_space) {
    return Status::Error(StatusCode::kPageFull, "页剩余空间不足");
  }

  // 写记录数据：free_end 向左挪 len 字节，把记录抄进去
  const uint16_t new_free_end = static_cast<uint16_t>(free_end - len);
  std::memcpy(page_->data() + new_free_end, record, len);

  // 写槽项：在第 slot_count 个槽位写入 {offset=新记录位置, len=记录长度}
  char* slot = page_->data() + kPageHeaderSize + slot_count * kSlotSize;
  PutUint16(slot + kSlotOffset, new_free_end);
  PutUint16(slot + kSlotLen, len);

  // 更新页头：free_end 前移、slot_count +1
  page_->SetFreeEnd(new_free_end);
  page_->SetSlotCount(slot_count + 1);
  *out = slot_count;                    // 新记录的槽号 = 旧 slot_count
  return Status::OK();
}

Status SlottedPageLayout::GetRecord(slot_id_t slot, const char** data, uint16_t* len) {
  if (slot >= page_->GetSlotCount()) {
    return Status::Error(StatusCode::kInvalidArgument, "槽号越界");
  }
  // 读槽项 {offset, len}
  const char* s = page_->data() + kPageHeaderSize + static_cast<size_t>(slot) * kSlotSize;
  const uint16_t offset = GetUint16(s + kSlotOffset);
  const uint16_t length = GetUint16(s + kSlotLen);
  if (length == 0) {
    return Status::Error(StatusCode::kInvalidArgument, "记录已删除（墓碑）");
  }
  *data = page_->data() + offset;       // 记录数据在页内的真实位置
  *len  = length;
  return Status::OK();
}

Status SlottedPageLayout::DeleteRecord(slot_id_t slot) {
  if (slot >= page_->GetSlotCount()) {
    return Status::Error(StatusCode::kInvalidArgument, "槽号越界");
  }
  // 标记删除：只把槽项的 len 置 0（墓碑），不回收空间、不移动其它记录。
  // 扫描时遇到 len==0 就跳过。
  char* s = page_->data() + kPageHeaderSize + static_cast<size_t>(slot) * kSlotSize;
  PutUint16(s + kSlotLen, 0);
  return Status::OK();
}

uint16_t SlottedPageLayout::GetFreeSpace() const {
  const uint16_t slot_count = page_->GetSlotCount();
  const uint16_t free_end   = page_->GetFreeEnd();
  const size_t slot_array_end = kPageHeaderSize + static_cast<size_t>(slot_count) * kSlotSize;
  if (free_end <= slot_array_end) {
    return 0;
  }
  return static_cast<uint16_t>(free_end - slot_array_end);
}

}  // namespace cella::storage
