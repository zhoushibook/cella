#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "i_storage.h"

namespace cella::storage {

class BufferPoolManager;
class IDiskManager;
class ILogger;
class IRecordSerializer;

// ── File 存储实现：文件磁盘后端 + 缓冲池 + 表目录 ────────────
// 表目录（name → schema + 首数据页）持久化在 kCatalogPage，由 MetaPage.catalog_root_page 引用。
class FileStorage : public IStorage {
 public:
  FileStorage() = default;
  ~FileStorage() override;

  FileStorage(const FileStorage&) = delete;
  FileStorage& operator=(const FileStorage&) = delete;

  const char* InterfaceVersion() const override { return kInterfaceVersion; }

  Status Open(const StorageConfig& config) override;
  void   Close() override;

  Status read_page(page_id_t, char*) override;
  Status write_page(page_id_t, const char*) override;
  Page*  get_page(page_id_t) override;
  Status flush_page(page_id_t) override;
  Status unpin_page(page_id_t, bool is_dirty) override;
  Status allocate_page(page_id_t* out) override;
  Status free_page(page_id_t) override;

  Status create_table(const std::string& name, const Schema& schema) override;
  Status drop_table(const std::string& name) override;
  Status open_table(const std::string& name, std::shared_ptr<TableHeap>* out) override;
  Status insert_record(const std::string& table, const Record& record, Rid* out) override;
  Status delete_record(const std::string& table, const Rid& rid) override;

  const BufferStats& get_stats() const override;
  std::string dump_stats() const override;
  std::vector<std::string> recent_evictions() const override;

 private:
  struct TableEntry {
    Schema schema;
    page_id_t first_page_id = kInvalidPageId;
  };

  Status load_directory();
  Status persist_directory();

  bool opened_ = false;
  StorageConfig config_;
  IDiskManager* disk_ = nullptr;            // 裸指针，实际由 bpm_ 持有
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<IRecordSerializer> serializer_;
  std::unique_ptr<ILogger> logger_;
  std::unordered_map<std::string, TableEntry> tables_;
  std::unordered_map<std::string, std::shared_ptr<TableHeap>> open_tables_;   // 缓存，保持 hint 有效
};

}  // namespace cella::storage
