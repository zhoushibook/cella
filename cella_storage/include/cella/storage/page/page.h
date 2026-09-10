#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "cella/storage/common/types.h"

namespace cella::storage {

constexpr size_t kPageHeaderSize = 32;

// ── 页头字段偏移（§8 二进制格式，小端）──────────────────────
// 0 page_id:u32  4 page_type:u16  6 prev_page_id:u32  10 next_page_id:u32
// 14 slot_count:u16  16 free_end:u16  18~27 reserved  28 checksum:u32
namespace page_header {
constexpr size_t kPageId     = 0;    // u32
constexpr size_t kPageType   = 4;    // u16
constexpr size_t kPrevPageId = 6;    // u32
constexpr size_t kNextPageId = 10;   // u32
constexpr size_t kSlotCount  = 14;   // u16
constexpr size_t kFreeEnd    = 16;   // u16
constexpr size_t kChecksum   = 28;   // u32
}  // namespace page_header

// ── 页对象：vector<char> 支持可配置页大小（约束 #2）──────────
class Page {
 public:
  Page() = default;
  explicit Page(uint32_t page_size) : data_(page_size, 0), page_size_(page_size) {}

  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;
  Page(Page&&) = default;
  Page& operator=(Page&&) = default;

  char* data() { return data_.data(); }
  const char* data() const { return data_.data(); }
  uint32_t page_size() const { return page_size_; }
  page_id_t page_id() const { return page_id_; }
  void set_page_id(page_id_t id) { page_id_ = id; }
  void reset_memory() { data_.assign(data_.size(), 0); }

  // 页头字段读写（小端，直接操作缓冲区）
  page_id_t GetHeaderPageId() const;
  void      SetHeaderPageId(page_id_t id);
  PageType  GetHeaderPageType() const;
  void      SetHeaderPageType(PageType t);
  page_id_t GetHeaderPrevPageId() const;
  void      SetHeaderPrevPageId(page_id_t id);
  page_id_t GetHeaderNextPageId() const;
  void      SetHeaderNextPageId(page_id_t id);
  uint16_t  GetSlotCount() const;
  void      SetSlotCount(uint16_t n);
  uint16_t  GetFreeEnd() const;
  void      SetFreeEnd(uint16_t end);
  uint32_t  GetChecksum() const;
  void      SetChecksum(uint32_t c);

 private:
  std::vector<char> data_;
  uint32_t page_size_ = 0;
  page_id_t page_id_ = kInvalidPageId;
};

}  // namespace cella::storage
