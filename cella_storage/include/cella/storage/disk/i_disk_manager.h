#pragma once
#include <cstdint>
#include <string>
#include "cella/storage/common/status.h"
#include "cella/storage/common/types.h"

namespace cella::storage {

// ── 磁盘后端抽象：File/Mem 两种实现；新增后端 = 新文件实现本接口 ──
// 页偏移公式：offset(page_id) = page_id * page_size（page_id=0 为 MetaPage）
class IDiskManager {
 public:
  virtual ~IDiskManager() = default;

  virtual Status    Open(const std::string& path) = 0;
  virtual Status    Close() = 0;
  virtual Status    ReadPage(page_id_t page_id, char* data) = 0;
  virtual Status    WritePage(page_id_t page_id, const char* data) = 0;

  // 分配页：优先复用空闲页链表；失败（如磁盘满）返回 kInvalidPageId
  virtual page_id_t AllocatePage() = 0;
  virtual Status    FreePage(page_id_t page_id) = 0;

  virtual uint64_t  GetDiskReadCount() const = 0;
  virtual uint64_t  GetDiskWriteCount() const = 0;
  virtual uint64_t  GetPageCount() const = 0;   // 文件总页数（含 MetaPage）
  virtual uint32_t  GetPageSize() const = 0;    // 页大小（缓冲池据此构造页）

  // 目录根页（MetaPage.catalog_root_page，预留 kCatalogPage 给目录/引擎组）
  virtual page_id_t GetCatalogRootPage() const = 0;
  virtual Status    SetCatalogRootPage(page_id_t) = 0;
};

}  // namespace cella::storage
