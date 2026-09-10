#pragma once
#include <cstdint>

namespace cella::storage {

// ── 基本 ID 类型 ─────────────────────────────────────────────
using page_id_t  = uint32_t;   // 页号，全局唯一，0 = MetaPage
using slot_id_t  = uint16_t;   // 页内槽号
using frame_id_t = uint32_t;   // 缓冲池帧号
using offset_t   = uint16_t;   // 页内偏移（槽式布局 free_end 用 u16，单页 ≤64KB）

constexpr page_id_t kInvalidPageId = 0xFFFFFFFFu;
constexpr slot_id_t kInvalidSlotId = 0xFFFFu;
constexpr page_id_t kMetaPageId    = 0u;

// ── 页类型（预留目录页/索引页/溢出页）────────────────────────
enum class PageType : uint16_t {
  kMetaPage     = 0,
  kDataPage     = 1,
  kCatalogPage  = 2,   // 引擎组把 catalog 当特殊表存（预留 + 页级字节读写）
  kIndexPage    = 3,   // 预留 B+ 树
  kFreeListPage = 4,
  kOverflowPage = 5,   // 预留超长记录溢出
};

// ── 记录定位：某页某槽 ───────────────────────────────────────
struct Rid {
  page_id_t page_id = kInvalidPageId;
  slot_id_t slot_id = kInvalidSlotId;

  bool IsValid() const {
    return page_id != kInvalidPageId && slot_id != kInvalidSlotId;
  }
  bool operator==(const Rid& o) const {
    return page_id == o.page_id && slot_id == o.slot_id;
  }
  bool operator!=(const Rid& o) const { return !(*this == o); }
};

// ── 值类型集中 switch（新增类型只改此枚举 + types.cpp）────────
enum class ValueType : uint8_t {
  kNull    = 0,
  kBool    = 1,
  kInt32   = 2,
  kInt64   = 3,
  kFloat   = 4,
  kDouble  = 5,
  kVarchar = 6,
  kChar    = 7,
  kDate    = 8,   // 预留
};

const char* ToString(ValueType t);
uint16_t    ValueSize(ValueType t);   // 定长类型字节数；变长/Null 返回 0

}  // namespace cella::storage
