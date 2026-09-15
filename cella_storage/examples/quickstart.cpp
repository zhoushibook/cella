// 30 秒上手：打开库 → 建表 → 插一行 → 查出来（文档《INTERFACE_CONTRACT.md》§2 的真实源码）
#include <cstdio>
#include <filesystem>
#include <memory>

#include "i_storage.h"
#include "storage_factory.h"
#include "record.h"
#include "schema.h"
#include "value.h"
#include "table_heap.h"
#include "console_utf8.h"   // 扁平名：demo/examples 只挂了扁平公共目录

int main() {
  cella::storage::EnableUtf8Console();   // 控制台按 UTF-8 显示中文
  using namespace cella::storage;

  StorageConfig config;
  config.data_dir = "./quickstart_data";
  std::filesystem::remove_all(config.data_dir);

  auto storage = CreateStorage(config);
  if (!storage->Open(config).ok()) return 1;

  Schema schema;
  schema.AddColumn("id", ValueType::kInt32);
  schema.AddColumn("name", ValueType::kVarchar, 64);
  (void)storage->create_table("student", schema);

  Record rec;
  rec.AddValue(Value::Int(1));
  rec.AddValue(Value::Varchar("Alice"));
  Rid rid;
  (void)storage->insert_record("student", rec, &rid);

  std::shared_ptr<TableHeap> heap;
  (void)storage->open_table("student", &heap);
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    std::printf("row: %s\n", it->ToString().c_str());
  }

  storage->Close();
  std::printf("quickstart OK\n");
  return 0;
}
