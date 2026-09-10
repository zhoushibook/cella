#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include "cella/storage/disk/i_disk_manager.h"

namespace cella::storage {

// 文件磁盘后端：文件即页数组，offset = page_id * page_size。
// 空闲页链表复用页头 next_page_id，头指针存 MetaPage（page_id=0）。
class FileDiskManager : public IDiskManager {
 public:
  FileDiskManager() = default;
  explicit FileDiskManager(uint32_t page_size) : page_size_(page_size) {}
  ~FileDiskManager() override;

  FileDiskManager(const FileDiskManager&) = delete;
  FileDiskManager& operator=(const FileDiskManager&) = delete;

  Status    Open(const std::string& path) override;
  Status    Close() override;
  Status    ReadPage(page_id_t page_id, char* data) override;
  Status    WritePage(page_id_t page_id, const char* data) override;
  page_id_t AllocatePage() override;
  Status    FreePage(page_id_t page_id) override;

  uint64_t GetDiskReadCount() const override { return read_count_; }
  uint64_t GetDiskWriteCount() const override { return write_count_; }
  uint64_t GetPageCount() const override { return page_count_; }
  uint32_t GetPageSize() const override { return page_size_; }

  page_id_t GetCatalogRootPage() const override { return catalog_root_page_; }
  Status    SetCatalogRootPage(page_id_t p) override;

 private:
  Status WriteMetaPage();   // 持久化 MetaPage（page_count / free_list_head）
  Status ReadMetaPage();    // 加载并校验 MetaPage

  uint32_t page_size_ = 4096;
  std::fstream file_;
  uint64_t read_count_ = 0;
  uint64_t write_count_ = 0;
  uint32_t page_count_ = 0;             // 文件总页数（含 MetaPage）
  uint32_t free_list_head_ = kInvalidPageId;
  uint32_t catalog_root_page_ = kInvalidPageId;
};

}  // namespace cella::storage
