#pragma once
#include <memory>
#include "record.h"
#include "schema.h"
#include "status.h"
#include "types.h"
#include "free_space_manager.h"
#include "table_iterator.h"

namespace cella::storage {

class BufferPoolManager;
class IRecordSerializer;

// ── 一张表 = 一条数据页双向链表（§8）────────────────────────
// 插入：页满则自动 AllocatePage 挂链表尾；删除为标记删除（槽 len 置 0）。
class TableHeap {
 public:
  TableHeap(BufferPoolManager* bpm, page_id_t first_page_id, const Schema& schema,
            IRecordSerializer* serializer);

  TableHeap(const TableHeap&) = delete;
  TableHeap& operator=(const TableHeap&) = delete;

  Status InsertRecord(const Record& record, Rid* out);
  Status GetRecord(const Rid& rid, Record* out);
  Status DeleteRecord(const Rid& rid);

  TableIterator begin();
  TableIterator end();

  page_id_t first_page_id() const { return first_page_id_; }
  const Schema& schema() const { return schema_; }

 private:
  BufferPoolManager* bpm_;
  page_id_t first_page_id_;
  Schema schema_;
  IRecordSerializer* serializer_;
  FreeSpaceManager fsm_;
};

}  // namespace cella::storage
