#include "cella/storage/api/file_storage.h"

#include <cstring>
#include <filesystem>
#include <vector>

#include "cella/storage/api/storage_factory.h"
#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/buffer/replacer_factory.h"
#include "cella/storage/common/byte_buffer.h"
#include "cella/storage/common/logger.h"
#include "cella/storage/disk/file_disk_manager.h"
#include "cella/storage/disk/i_disk_manager.h"
#include "cella/storage/page/page.h"
#include "cella/storage/record/slotted_record_serializer.h"
#include "cella/storage/table/table_heap.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// FileStorage：把下面几个组件组装成对外统一接口 IStorage 的具体实现。
//
//   磁盘后端 FileDiskManager ──┐
//   淘汰策略 Replacer      ────┼──► BufferPoolManager（缓冲池，唯一页 I/O 出口）
//   记录序列化 SlottedRecordSerializer ─┘
//   表目录 tables_：表名 → (schema, 首数据页号)，持久化在 kCatalogPage
//
// 关键生命周期：
//   Open  → 开磁盘 + 组缓冲池 + 读目录（load_directory）
//   Close → 写回目录（persist_directory）+ 刷脏页（FlushAllPages）
// ─────────────────────────────────────────────────────────────────────────

std::unique_ptr<IStorage> CreateStorage(const StorageConfig& config) {
  (void)config;                          // 当前只有一个实现，忽略配置；将来可据此分发
  return std::make_unique<FileStorage>();
}

FileStorage::~FileStorage() { Close(); }

Status FileStorage::Open(const StorageConfig& config) {
  if (opened_) {
    return Status::OK();                 // 幂等：重复 Open 不报错
  }
  const Status v = config.Validate();    // 校验 page_size 等配置
  if (!v.ok()) {
    return v;
  }
  config_ = config;

  // 1. 打开磁盘后端（文件即数据库本体；不存在会自动创建）
  std::filesystem::create_directories(config_.data_dir);
  const std::string path = config_.data_dir + "/" + config_.db_file;

  auto disk = std::make_unique<FileDiskManager>(config_.page_size);
  Status s = disk->Open(path);
  if (!s.ok()) {
    return s;
  }
  disk_ = disk.get();                     // 记住裸指针：所有权马上要交给 bpm_

  // 2. 按配置名创建淘汰策略 + 组装缓冲池
  auto replacer = ReplacerFactory::Create(config_.replacer, config_.pool_size);
  if (replacer == nullptr) {
    return Status::Error(StatusCode::kInvalidConfig,
                         "未知替换策略: " + config_.replacer);
  }
  bpm_ = std::make_unique<BufferPoolManager>(config_.pool_size, std::move(disk),
                                             std::move(replacer));
  serializer_ = std::make_unique<SlottedRecordSerializer>();

  // 3. 日志（可选）：Console 或 File 后端
  if (config_.enable_log) {
    if (config_.log_path.empty()) {
      logger_ = std::make_unique<ConsoleLogger>();
    } else {
      logger_ = std::make_unique<FileLogger>(config_.log_path);
    }
    bpm_->SetLogger(logger_.get());
  }

  opened_ = true;
  return load_directory();                // 4. 从磁盘读回表目录（重启后表还在）
}

void FileStorage::Close() {
  if (!opened_) {
    return;
  }
  (void)persist_directory();              // 目录写回缓冲（标记目录页为脏）
  if (bpm_) {
    bpm_->FlushAllPages();                // 把所有脏页（含目录页）真正写盘
  }
  open_tables_.clear();                   // 清掉缓存的 TableHeap
  opened_ = false;
}

// ── 页级读写：全部委托给缓冲池 ──────────────────────────────
Status FileStorage::read_page(page_id_t pid, char* buf) { return bpm_->read_page(pid, buf); }
Status FileStorage::write_page(page_id_t pid, const char* buf) { return bpm_->write_page(pid, buf); }
Page*  FileStorage::get_page(page_id_t pid) { return bpm_->get_page(pid); }

Status FileStorage::flush_page(page_id_t pid) {
  return bpm_->flush_page(pid) ? Status::OK()
                               : Status::Error(StatusCode::kPageNotFound, "flush 失败");
}

Status FileStorage::unpin_page(page_id_t pid, bool is_dirty) {
  return bpm_->UnpinPage(pid, is_dirty) ? Status::OK()
                                        : Status::Error(StatusCode::kPageNotFound, "unpin 失败");
}

Status FileStorage::allocate_page(page_id_t* out) {
  Page* p = bpm_->NewPage(out);
  return p == nullptr ? Status::Error(StatusCode::kNoFreeFrame, "无可用帧") : Status::OK();
}

Status FileStorage::free_page(page_id_t pid) { return disk_->FreePage(pid); }

Status FileStorage::create_table(const std::string& name, const Schema& schema) {
  if (!opened_) {
    return Status::Error(StatusCode::kInvalidArgument, "未打开");
  }
  if (tables_.count(name) != 0) {
    return Status::Error(StatusCode::kTableAlreadyExists, "表已存在: " + name);
  }
  if (schema.column_count() == 0) {
    return Status::Error(StatusCode::kInvalidArgument, "空 schema");
  }

  // 分配这张表的「首数据页」，初始化页头（prev/next 都空、0 条记录）
  page_id_t first = kInvalidPageId;
  Page* p = bpm_->NewPage(&first);
  if (p == nullptr) {
    return Status::Error(StatusCode::kNoFreePage, "无法分配数据页");
  }
  {
    PageGuard pg(bpm_.get(), p);
    pg->SetHeaderPageId(first);
    pg->SetHeaderPageType(PageType::kDataPage);
    pg->SetHeaderPrevPageId(kInvalidPageId);
    pg->SetHeaderNextPageId(kInvalidPageId);
    pg->SetSlotCount(0);
    pg->SetFreeEnd(static_cast<uint16_t>(pg->page_size()));
    pg.MarkDirty();
  }

  // 登记目录，并立即持久化（保证重启后表还在）
  TableEntry entry;
  entry.schema = schema;
  entry.first_page_id = first;
  tables_[name] = std::move(entry);

  return persist_directory();
}

Status FileStorage::drop_table(const std::string& name) {
  if (!opened_) {
    return Status::Error(StatusCode::kInvalidArgument, "未打开");
  }
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    return Status::Error(StatusCode::kTableNotFound, "表不存在: " + name);
  }

  // 沿数据页链表走一遍，把每一页都释放回空闲链表
  page_id_t pid = it->second.first_page_id;
  while (pid != kInvalidPageId) {
    page_id_t next = kInvalidPageId;
    {
      PageGuard pg(bpm_.get(), bpm_->get_page(pid));
      if (pg.valid()) {
        next = pg->GetHeaderNextPageId();   // 先记下下一页，再释放当前页
      }
    }
    (void)disk_->FreePage(pid);
    pid = next;
  }
  tables_.erase(it);
  open_tables_.erase(name);
  return persist_directory();
}

Status FileStorage::open_table(const std::string& name, std::shared_ptr<TableHeap>* out) {
  if (!opened_) {
    return Status::Error(StatusCode::kInvalidArgument, "未打开");
  }
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    return Status::Error(StatusCode::kTableNotFound, "表不存在: " + name);
  }
  // 复用已打开的 TableHeap：保持它内部的「插入目标页提示」有效。
  // （若每次都新建，提示会被重置到首页，导致插入性能退化 / 链被搞乱）
  auto cached = open_tables_.find(name);
  if (cached != open_tables_.end()) {
    *out = cached->second;
    return Status::OK();
  }
  auto heap = std::make_shared<TableHeap>(bpm_.get(), it->second.first_page_id,
                                          it->second.schema, serializer_.get());
  open_tables_[name] = heap;
  *out = std::move(heap);
  return Status::OK();
}

Status FileStorage::insert_record(const std::string& table, const Record& record, Rid* out) {
  std::shared_ptr<TableHeap> heap;
  const Status s = open_table(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->InsertRecord(record, out);
}

Status FileStorage::delete_record(const std::string& table, const Rid& rid) {
  std::shared_ptr<TableHeap> heap;
  const Status s = open_table(table, &heap);
  if (!s.ok()) {
    return s;
  }
  return heap->DeleteRecord(rid);
}

const BufferStats& FileStorage::get_stats() const { return bpm_->GetStats(); }
std::string FileStorage::dump_stats() const { return bpm_->GetStats().ToString(); }

std::vector<std::string> FileStorage::recent_evictions() const {
  const auto& q = bpm_->recent_evictions();
  return std::vector<std::string>(q.begin(), q.end());
}

// ─────────────────────────────────────────────────────────────────────────
// 目录持久化：把 tables_（表名 → schema + 首数据页）序列化进一个 kCatalogPage。
//
// 目录页内格式：
//   [blob_len:u32][blob]，blob = 各表逐个序列化：
//     [表数:u32]
//     每表：[表名:u16+字节][首页号:u32][列数:u16]
//           每列：[列名:u16+字节][类型:u16][max_len:u16]
//
// 目录页号存在 MetaPage.catalog_root_page 里，重启时据此找回。
// 注意：catalog 的「富语义」（索引、约束、统计）归引擎组，这里只存最小编码。
// ─────────────────────────────────────────────────────────────────────────
Status FileStorage::persist_directory() {
  // 1. 把 tables_ 序列化成 blob
  ByteBuffer bb;
  bb.PutUint32(static_cast<uint32_t>(tables_.size()));
  for (const auto& kv : tables_) {
    const TableEntry& entry = kv.second;
    bb.PutString(kv.first);                            // 表名
    bb.PutUint32(entry.first_page_id);                 // 首数据页
    bb.PutUint16(static_cast<uint16_t>(entry.schema.column_count()));
    for (size_t i = 0; i < entry.schema.column_count(); ++i) {
      const Column& c = entry.schema.column(i);
      bb.PutString(c.name);                            // 列名
      bb.PutUint16(static_cast<uint16_t>(c.type));     // 列类型
      bb.PutUint16(c.max_len);                         // varchar 最大长度
    }
  }
  std::vector<char> blob = std::move(bb.data());

  // 2. 确保有目录页：没有就分配一页并登记到 MetaPage
  page_id_t catalog_page = disk_->GetCatalogRootPage();
  if (catalog_page == kInvalidPageId) {
    Page* p = bpm_->NewPage(&catalog_page);
    if (p == nullptr) {
      return Status::Error(StatusCode::kNoFreePage, "无法分配目录页");
    }
    {
      PageGuard pg(bpm_.get(), p);
      pg->SetHeaderPageId(catalog_page);
      pg->SetHeaderPageType(PageType::kCatalogPage);
      pg.MarkDirty();
    }
    const Status s = disk_->SetCatalogRootPage(catalog_page);   // 立即写回 MetaPage
    if (!s.ok()) {
      return s;
    }
  }

  // 3. 把 blob 写进目录页（标记脏，真正落盘交给 Close 的 FlushAllPages）
  if (blob.size() + 4 > config_.page_size) {
    return Status::Error(StatusCode::kNotImplemented, "目录超过单页容量（暂不支持）");
  }
  PageGuard pg(bpm_.get(), bpm_->get_page(catalog_page));
  if (!pg.valid()) {
    return Status::Error(StatusCode::kIoError, "读取目录页失败");
  }
  PutUint32(pg->data(), static_cast<uint32_t>(blob.size()));
  std::memcpy(pg->data() + 4, blob.data(), blob.size());
  pg.MarkDirty();
  return Status::OK();
}

Status FileStorage::load_directory() {
  tables_.clear();
  const page_id_t catalog_page = disk_->GetCatalogRootPage();
  if (catalog_page == kInvalidPageId) {
    return Status::OK();                       // 全新库：还没有目录页
  }
  std::vector<char> page(config_.page_size, 0);
  const Status s = read_page(catalog_page, page.data());
  if (!s.ok()) {
    return s;
  }
  const uint32_t blob_len = GetUint32(page.data());
  if (blob_len == 0 || blob_len + 4 > config_.page_size) {
    return Status::OK();                       // 空/异常目录，当空库处理
  }
  // 反序列化各表（与 persist_directory 的顺序严格对应）
  ByteBuffer bb(std::vector<char>(page.data() + 4, page.data() + 4 + blob_len));
  const uint32_t table_count = bb.GetUint32();
  for (uint32_t t = 0; t < table_count; ++t) {
    const std::string name = bb.GetString();
    const page_id_t first_page = bb.GetUint32();
    const uint16_t col_count = bb.GetUint16();
    TableEntry entry;
    entry.first_page_id = first_page;
    for (uint16_t i = 0; i < col_count; ++i) {
      const std::string col_name = bb.GetString();
      const uint16_t col_type = bb.GetUint16();
      const uint16_t col_max_len = bb.GetUint16();
      entry.schema.AddColumn(col_name, static_cast<ValueType>(col_type), col_max_len);
    }
    tables_[name] = std::move(entry);
  }
  return Status::OK();
}

}  // namespace cella::storage
