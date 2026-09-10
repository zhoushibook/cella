// 全流程：建表 → 插入 → 扫描 → 过滤 → 投影 → 删除（文档《INTERFACE_CONTRACT.md》§4 的真实源码）
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "i_storage.h"
#include "storage_factory.h"
#include "record.h"
#include "schema.h"
#include "value.h"
#include "table_heap.h"

int main() {
  using namespace cella::storage;

  StorageConfig config;
  config.data_dir = "./crud_data";
  std::filesystem::remove_all(config.data_dir);

  auto storage = CreateStorage(config);
  if (!storage->Open(config).ok()) return 1;

  // 建表
  Schema schema;
  schema.AddColumn("id", ValueType::kInt32);
  schema.AddColumn("name", ValueType::kVarchar, 64);
  schema.AddColumn("age", ValueType::kInt32);
  (void)storage->create_table("student", schema);

  // 插入
  std::vector<Rid> rids;
  for (int i = 0; i < 10; ++i) {
    Record rec;
    rec.AddValue(Value::Int(i));
    rec.AddValue(Value::Varchar("stu" + std::to_string(i)));
    rec.AddValue(Value::Int(20 + i));
    Rid rid;
    (void)storage->insert_record("student", rec, &rid);
    rids.push_back(rid);
  }

  // 扫描 + 过滤（Filter: age > 24）
  int scanned = 0, filtered = 0;
  std::shared_ptr<TableHeap> heap;
  (void)storage->open_table("student", &heap);
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    ++scanned;
    if (it->value(2).int32_val > 24) {
      ++filtered;
    }
  }
  std::printf("扫描 %d 行, age>24 过滤 %d 行\n", scanned, filtered);

  // 投影前 3 行（Project）
  int n = 0;
  for (auto it = heap->begin(); it != heap->end() && n < 3; ++it, ++n) {
    std::printf("project: id=%d name=%s\n", it->value(0).int32_val,
                it->value(1).str_val.c_str());
  }

  // 删除（标记删除 id=0 的那一行）
  (void)storage->delete_record("student", rids[0]);
  int after = 0;
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    ++after;
  }
  std::printf("删除 1 行后剩余 %d 行\n", after);

  storage->Close();
  std::printf("crud OK\n");
  return 0;
}
