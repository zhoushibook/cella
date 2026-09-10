#include "cella/storage/page/meta_page.h"

#include <cstring>

#include "cella/storage/common/byte_buffer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// MetaPage（元信息页）的编解码。它是整个数据库文件的第 0 页（page_id=0），
// 固定 32 字节，记录整个库的全局信息。
//
// 布局（小端）：
//   offset 0   magic "CELLADB1"(8B)   识别「这是不是 cella 的数据文件」
//   offset 8   format_version:u32     数据文件格式版本（旧版本会被拒）
//   offset 12  page_size:u32          页大小
//   offset 16  page_count:u32         文件总页数（含 MetaPage 自身）
//   offset 20  free_list_head:u32     空闲页链表头（kInvalidPageId=无空闲页）
//   offset 24  catalog_root_page:u32  目录页号（表→页映射存在这页）
//   offset 28  checksum:u32           CRC32（预留，当前为 0）
//
// 它由磁盘层（FileDiskManager/MemDiskManager）读写，用来在重启后找回
// 页数、空闲链表、目录位置。magic 和 format_version 让「文件不是我们的」或
// 「是旧版本」能被立刻识别，而不是表现为随机崩溃（约束 #2.2 约定 3）。
// ─────────────────────────────────────────────────────────────────────────

void MetaPage::Encode(char* dst) const {
  std::memcpy(dst, kMetaMagic, 8);   // 写 magic（8 字节字面量 "CELLADB1"）
  PutUint32(dst + 8,  format_version);
  PutUint32(dst + 12, page_size);
  PutUint32(dst + 16, page_count);
  PutUint32(dst + 20, free_list_head);
  PutUint32(dst + 24, catalog_root_page);
  PutUint32(dst + 28, checksum);
}

Status MetaPage::Decode(const char* src, MetaPage* out) {
  // 先验 magic：不匹配说明这不是 cella 数据文件（或文件已损坏）
  if (std::memcmp(src, kMetaMagic, 8) != 0) {
    return Status::Error(StatusCode::kCorruptPage, "MetaPage magic 不匹配");
  }
  out->format_version    = GetUint32(src + 8);
  out->page_size         = GetUint32(src + 12);
  out->page_count        = GetUint32(src + 16);
  out->free_list_head    = GetUint32(src + 20);
  out->catalog_root_page = GetUint32(src + 24);
  out->checksum          = GetUint32(src + 28);
  return Status::OK();
}

}  // namespace cella::storage
