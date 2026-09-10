#include "cella/storage/page/page.h"

#include "cella/storage/common/byte_buffer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// Page 页头字段的读写。每个字段都存页 buffer 的固定偏移处（小端），
// 偏移定义见 page.h 里的 page_header 命名空间。
//
// 页头 32 字节布局（§8）：
//   offset 0  page_id:u32      这一页自己的页号
//   offset 4  page_type:u16    页类型（Data/Catalog/Index/FreeList…）
//   offset 6  prev_page_id:u32 同表链表的前一页
//   offset 10 next_page_id:u32 同表链表的下一页
//   offset 14 slot_count:u16   页内已用槽数
//   offset 16 free_end:u16     空闲区起始偏移（记录从这里向前摆）
//   offset 28 checksum:u32     CRC32（预留，当前为 0）
// ─────────────────────────────────────────────────────────────────────────

page_id_t Page::GetHeaderPageId() const { return GetUint32(data_.data() + page_header::kPageId); }
void      Page::SetHeaderPageId(page_id_t id) { PutUint32(data_.data() + page_header::kPageId, id); }

PageType Page::GetHeaderPageType() const {
  return static_cast<PageType>(GetUint16(data_.data() + page_header::kPageType));
}
void Page::SetHeaderPageType(PageType t) {
  PutUint16(data_.data() + page_header::kPageType, static_cast<uint16_t>(t));
}

page_id_t Page::GetHeaderPrevPageId() const { return GetUint32(data_.data() + page_header::kPrevPageId); }
void      Page::SetHeaderPrevPageId(page_id_t id) { PutUint32(data_.data() + page_header::kPrevPageId, id); }

page_id_t Page::GetHeaderNextPageId() const { return GetUint32(data_.data() + page_header::kNextPageId); }
void      Page::SetHeaderNextPageId(page_id_t id) { PutUint32(data_.data() + page_header::kNextPageId, id); }

uint16_t Page::GetSlotCount() const { return GetUint16(data_.data() + page_header::kSlotCount); }
void     Page::SetSlotCount(uint16_t n) { PutUint16(data_.data() + page_header::kSlotCount, n); }

uint16_t Page::GetFreeEnd() const { return GetUint16(data_.data() + page_header::kFreeEnd); }
void     Page::SetFreeEnd(uint16_t end) { PutUint16(data_.data() + page_header::kFreeEnd, end); }

uint32_t Page::GetChecksum() const { return GetUint32(data_.data() + page_header::kChecksum); }
void     Page::SetChecksum(uint32_t c) { PutUint32(data_.data() + page_header::kChecksum, c); }

}  // namespace cella::storage
