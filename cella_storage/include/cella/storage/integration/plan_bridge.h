#pragma once
#include <functional>
#include <memory>
#include <string>

#include "cella/storage/api/i_storage.h"
#include "cella/storage/table/table_heap.h"

namespace cella::storage {

// ── PlanBridge（stub）：演示数据库引擎各算子 → 存储接口的调用映射 ──
// 引擎组（组员B）拿到逻辑执行计划后，按下列方式调用存储系统。
// Filter / Project 属引擎组内存内运算（对扫描结果做谓词过滤、投影、取前 N），
// 存储系统不感知，只提供原始全表扫描。

// SeqScan 算子 → open_table + TableHeap::begin/end
inline Status SeqScan(IStorage* s, const std::string& table,
                      const std::function<void(const Record&, const Rid&)>& emit) {
  std::shared_ptr<TableHeap> heap;
  Status st = s->open_table(table, &heap);
  if (!st.ok()) {
    return st;
  }
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    emit(*it, it.rid());
  }
  return Status::OK();
}

// Insert 算子 → insert_record（建表由 DDL 阶段 create_table 完成）
inline Status Insert(IStorage* s, const std::string& table, const Record& rec, Rid* out) {
  return s->insert_record(table, rec, out);
}

// Delete 算子 → delete_record（标记删除）
inline Status Delete(IStorage* s, const std::string& table, const Rid& rid) {
  return s->delete_record(table, rid);
}

}  // namespace cella::storage
