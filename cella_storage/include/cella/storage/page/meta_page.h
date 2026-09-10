#pragma once
#include <cstddef>
#include <cstdint>
#include "cella/storage/common/status.h"
#include "cella/storage/common/types.h"
#include "cella/storage/common/version.h"

namespace cella::storage {

// ── MetaPage 二进制布局（page_id=0，32 字节，小端）───────────
//  0  magic "CELLADB1"(8B)  8 format_version:u32  12 page_size:u32
// 16 page_count:u32  20 free_list_head:u32  24 catalog_root_page:u32  28 checksum:u32
constexpr size_t kMetaPageSize = 32;
constexpr const char kMetaMagic[] = "CELLADB1";

struct MetaPage {
  uint32_t format_version    = kFormatVersion;
  uint32_t page_size         = 4096;
  uint32_t page_count        = 1;                 // 至少含 MetaPage 自身
  uint32_t free_list_head    = kInvalidPageId;
  uint32_t catalog_root_page = kInvalidPageId;
  uint32_t checksum          = 0;

  void   Encode(char* dst) const;                 // 写 32 字节（小端）
  static Status Decode(const char* src, MetaPage* out);   // 校验 magic
};

}  // namespace cella::storage
