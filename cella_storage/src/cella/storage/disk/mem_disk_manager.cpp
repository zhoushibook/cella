#include "cella/storage/disk/mem_disk_manager.h"

#include <cstring>

#include "cella/storage/common/byte_buffer.h"
#include "cella/storage/common/version.h"
#include "cella/storage/page/meta_page.h"
#include "cella/storage/page/page.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// MemDiskManager：内存版磁盘后端，专供测试用。
// 行为和 FileDiskManager 完全一致（同样的空闲页链表、同样的页号规则），
// 只是把「文件」换成 vector<vector<char>>，不落盘、更快。
// 好处：单测缓冲池时不用碰真实文件系统，结果却和文件版一致。
// ─────────────────────────────────────────────────────────────────────────

Status MemDiskManager::Open(const std::string& path) {
  (void)path;   // 内存后端忽略路径
  pages_.clear();
  free_list_head_ = kInvalidPageId;
  catalog_root_page_ = kInvalidPageId;

  // 初始化 MetaPage（pages_[0]），和文件版的「新文件初始化」对应
  MetaPage meta;
  meta.format_version = kFormatVersion;
  meta.page_size      = page_size_;
  meta.page_count     = 1;
  meta.free_list_head = kInvalidPageId;
  meta.catalog_root_page = kInvalidPageId;
  meta.checksum       = 0;

  std::vector<char> page0(page_size_, 0);
  meta.Encode(page0.data());
  pages_.push_back(std::move(page0));
  read_count_ = 0;
  write_count_ = 0;
  return Status::OK();
}

Status MemDiskManager::Close() { return Status::OK(); }

Status MemDiskManager::ReadPage(page_id_t page_id, char* data) {
  if (page_id >= pages_.size()) {
    return Status::Error(StatusCode::kPageNotFound, "页不存在: " + std::to_string(page_id));
  }
  std::memcpy(data, pages_[page_id].data(), page_size_);
  ++read_count_;
  return Status::OK();
}

Status MemDiskManager::WritePage(page_id_t page_id, const char* data) {
  if (page_id >= pages_.size()) {
    return Status::Error(StatusCode::kPageNotFound, "页不存在: " + std::to_string(page_id));
  }
  std::memcpy(pages_[page_id].data(), data, page_size_);
  ++write_count_;
  return Status::OK();
}

page_id_t MemDiskManager::AllocatePage() {
  page_id_t out = kInvalidPageId;
  if (free_list_head_ != kInvalidPageId) {
    // 复用空闲链表头：读它的 next 前移链表头
    out = free_list_head_;
    free_list_head_ = GetUint32(pages_[out].data() + page_header::kNextPageId);
  } else {
    // 追加新页：页号 = 当前页数
    out = static_cast<page_id_t>(pages_.size());
    pages_.emplace_back(page_size_, '\0');
  }
  // 把更新后的元信息写回 pages_[0]（MetaPage）
  char meta[kMetaPageSize];
  MetaPage m;
  m.format_version = kFormatVersion;
  m.page_size      = page_size_;
  m.page_count     = static_cast<uint32_t>(pages_.size());
  m.free_list_head = free_list_head_;
  m.catalog_root_page = catalog_root_page_;
  m.checksum       = 0;
  m.Encode(meta);
  std::memcpy(pages_[kMetaPageId].data(), meta, kMetaPageSize);
  ++write_count_;
  return out;
}

Status MemDiskManager::FreePage(page_id_t page_id) {
  if (page_id == kMetaPageId || page_id >= pages_.size()) {
    return Status::Error(StatusCode::kInvalidArgument, "非法页号: " + std::to_string(page_id));
  }
  // 该页清空，页头 next 指向原链表头，把它头插进空闲链表
  std::vector<char>& p = pages_[page_id];
  p.assign(page_size_, 0);
  PutUint32(p.data() + page_header::kNextPageId, free_list_head_);
  PutUint16(p.data() + page_header::kPageType, static_cast<uint16_t>(PageType::kFreeListPage));
  free_list_head_ = page_id;

  // 更新 MetaPage
  char meta[kMetaPageSize];
  MetaPage m;
  m.format_version = kFormatVersion;
  m.page_size      = page_size_;
  m.page_count     = static_cast<uint32_t>(pages_.size());
  m.free_list_head = free_list_head_;
  m.catalog_root_page = catalog_root_page_;
  m.checksum       = 0;
  m.Encode(meta);
  std::memcpy(pages_[kMetaPageId].data(), meta, kMetaPageSize);
  ++write_count_;
  return Status::OK();
}

Status MemDiskManager::SetCatalogRootPage(page_id_t p) {
  catalog_root_page_ = p;
  char meta[kMetaPageSize];
  MetaPage m;
  m.format_version = kFormatVersion;
  m.page_size      = page_size_;
  m.page_count     = static_cast<uint32_t>(pages_.size());
  m.free_list_head = free_list_head_;
  m.catalog_root_page = catalog_root_page_;
  m.checksum       = 0;
  m.Encode(meta);
  std::memcpy(pages_[kMetaPageId].data(), meta, kMetaPageSize);
  ++write_count_;
  return Status::OK();
}

}  // namespace cella::storage
