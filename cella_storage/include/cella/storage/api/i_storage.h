#pragma once
#include <memory>
#include <string>
#include <vector>

#include "buffer_stats.h"
#include "config.h"
#include "record.h"
#include "schema.h"
#include "status.h"
#include "types.h"

namespace cella::storage {

class Page;      // 内部句柄：引擎组一般无需直接使用（原始字节读写走 read_page/write_page）
class TableHeap; // 表访问接口：open_table 返回，用于扫描

// ── 统一对外接口：引擎组只依赖本头 + common/ 与 table/ ────────
// 线程安全：单线程设计；事务/并发由引擎组在上层自行处理。
class IStorage {
 public:
  static constexpr const char* kInterfaceVersion = "cella-storage/0.1";

  virtual ~IStorage() = default;
  virtual const char* InterfaceVersion() const { return kInterfaceVersion; }

  // ── 生命周期 ─────────────────────────────────────────────
  virtual Status Open(const StorageConfig&) = 0;   // 幂等；已打开返回 kOk
  virtual void   Close() = 0;                       // FlushAllPages + 关日志

  // ── 页级读写（指导书点名，签名不可改）────────────────────
  virtual Status read_page(page_id_t, char*) = 0;
  virtual Status write_page(page_id_t, const char*) = 0;
  virtual Page*  get_page(page_id_t) = 0;           // 返回已 pin 的页（配 unpin_page）
  virtual Status flush_page(page_id_t) = 0;
  virtual Status unpin_page(page_id_t, bool is_dirty) = 0;
  virtual Status allocate_page(page_id_t* out) = 0;
  virtual Status free_page(page_id_t) = 0;

  // ── 表级操作 ─────────────────────────────────────────────
  virtual Status create_table(const std::string& name, const Schema&) = 0;
  virtual Status drop_table(const std::string& name) = 0;
  virtual Status open_table(const std::string&, std::shared_ptr<TableHeap>* out) = 0;
  virtual Status insert_record(const std::string& table, const Record&, Rid* out) = 0;
  virtual Status delete_record(const std::string& table, const Rid&) = 0;

  // ── 统计 ────────────────────────────────────────────────
  virtual const BufferStats& get_stats() const = 0;
  virtual std::string dump_stats() const = 0;   // 表格化文本，供 CLI/demo 打印
  virtual std::vector<std::string> recent_evictions() const = 0;   // 最近替换日志（§10）
};

}  // namespace cella::storage
