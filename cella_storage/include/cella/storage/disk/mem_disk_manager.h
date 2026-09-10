#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "cella/storage/disk/i_disk_manager.h"

namespace cella::storage {

// 内存磁盘后端（测试用）：行为与 File 一致，但不落盘。
class MemDiskManager : public IDiskManager {
 public:
  MemDiskManager() = default;
  explicit MemDiskManager(uint32_t page_size) : page_size_(page_size) {}
  ~MemDiskManager() override = default;

  Status    Open(const std::string& path) override;   // path 忽略，重置为全新状态
  Status    Close() override;
  Status    ReadPage(page_id_t page_id, char* data) override;
  Status    WritePage(page_id_t page_id, const char* data) override;
  page_id_t AllocatePage() override;
  Status    FreePage(page_id_t page_id) override;

  uint64_t GetDiskReadCount() const override { return read_count_; }
  uint64_t GetDiskWriteCount() const override { return write_count_; }
  uint64_t GetPageCount() const override { return pages_.size(); }
  uint32_t GetPageSize() const override { return page_size_; }

  page_id_t GetCatalogRootPage() const override { return catalog_root_page_; }
  Status    SetCatalogRootPage(page_id_t p) override;

 private:
  uint32_t page_size_ = 4096;
  std::vector<std::vector<char>> pages_;   // pages_[0] 为 MetaPage
  uint64_t read_count_ = 0;
  uint64_t write_count_ = 0;
  uint32_t free_list_head_ = kInvalidPageId;
  uint32_t catalog_root_page_ = kInvalidPageId;
};

}  // namespace cella::storage
