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

  // ── 行数统计（P1.6 代价模型输入）─────────────────────────
  // 懒加载：首次调用沿链表数一遍并记住；此后 Insert/Delete 增量维护，
  // O(1) 返回。所有行变更都必须走 InsertRecord/DeleteRecord（存储层约定），
  // 计数才不会漂移；TRUNCATE/ALTER 走「drop + create + 回填」，新堆未知 →
  // 自然回落到懒扫描，无需特殊处理。
  size_t RowCount();

  page_id_t first_page_id() const { return first_page_id_; }
  const Schema& schema() const { return schema_; }

 private:
  BufferPoolManager* bpm_;
  page_id_t first_page_id_;
  Schema schema_;
  IRecordSerializer* serializer_;
  FreeSpaceManager fsm_;
  size_t row_count_ = 0;        // 惰性行数：仅在 row_count_known_ 为真时可信
  bool row_count_known_ = false;
};

}  // namespace cella::storage
