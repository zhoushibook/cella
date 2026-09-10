#pragma once
#include "record.h"
#include "schema.h"
#include "types.h"

namespace cella::storage {

class BufferPoolManager;
class IRecordSerializer;

// ── 跨页迭代器：扫描整张表，跳过墓碑（槽 len==0）─────────────
// 用法：for (auto it = heap.begin(); it != heap.end(); ++it) { *it ... }
class TableIterator {
 public:
  TableIterator() : done_(true) {}   // end 哨兵

  TableIterator(BufferPoolManager* bpm, page_id_t first_page, const Schema& schema,
                IRecordSerializer* serializer);

  const Record& operator*() const { return current_; }
  const Record* operator->() const { return &current_; }
  Rid rid() const { return current_rid_; }

  bool operator!=(const TableIterator& o) const { return done_ != o.done_; }
  bool operator==(const TableIterator& o) const { return done_ == o.done_; }

  TableIterator& operator++();

 private:
  void advance_to_valid();

  BufferPoolManager* bpm_ = nullptr;
  page_id_t page_id_ = kInvalidPageId;
  slot_id_t slot_id_ = 0;
  const Schema* schema_ = nullptr;
  IRecordSerializer* serializer_ = nullptr;
  Record current_;
  Rid current_rid_;
  bool done_ = true;
};

}  // namespace cella::storage
