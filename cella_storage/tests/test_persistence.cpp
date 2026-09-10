#include "mini_test.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cella/storage/api/i_storage.h"
#include "cella/storage/api/storage_factory.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/value.h"
#include "cella/storage/disk/file_disk_manager.h"
#include "cella/storage/page/meta_page.h"
#include "cella/storage/table/table_heap.h"

using namespace cella::storage;

namespace {

StorageConfig MakeConfig(const std::string& db_file) {
  StorageConfig c;
  c.data_dir = "./test_data";
  c.db_file = db_file;
  c.pool_size = 64;
  return c;
}

}  // namespace

// 持久化：Close → Open → 数据完整
TEST_CASE(persistence_close_open) {
  StorageConfig config = MakeConfig("persist.db");
  std::filesystem::remove_all(config.data_dir);
  const int N = 100;

  {
    auto storage = CreateStorage(config);
    EXPECT_TRUE(storage->Open(config).ok());
    Schema schema;
    schema.AddColumn("id", ValueType::kInt32);
    schema.AddColumn("name", ValueType::kVarchar, 64);
    EXPECT_TRUE(storage->create_table("student", schema).ok());
    for (int i = 0; i < N; ++i) {
      Record rec;
      rec.AddValue(Value::Int(i));
      rec.AddValue(Value::Varchar("n" + std::to_string(i)));
      Rid rid;
      EXPECT_TRUE(storage->insert_record("student", rec, &rid).ok());
    }
    storage->Close();
  }

  {
    auto storage = CreateStorage(config);
    EXPECT_TRUE(storage->Open(config).ok());
    std::shared_ptr<TableHeap> heap;
    EXPECT_TRUE(storage->open_table("student", &heap).ok());
    int count = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it) {
      EXPECT_EQ(it->value(0).int32_val, count);
      EXPECT_TRUE(it->value(1).str_val == "n" + std::to_string(count));
      ++count;
    }
    EXPECT_EQ(count, N);
    storage->Close();
  }

  std::filesystem::remove_all(config.data_dir);
}

// 版本不兼容返回 kVersionMismatch
TEST_CASE(version_mismatch) {
  StorageConfig config = MakeConfig("mismatch.db");
  std::filesystem::remove_all(config.data_dir);
  std::filesystem::create_directories(config.data_dir);
  const std::string path = config.data_dir + "/" + config.db_file;

  {
    FileDiskManager dm(4096);
    (void)dm.Open(path);
    MetaPage meta;
    meta.format_version = 999;   // 错误版本
    meta.page_size = 4096;
    meta.page_count = 1;
    meta.free_list_head = kInvalidPageId;
    meta.catalog_root_page = kInvalidPageId;
    std::vector<char> buf(4096, 0);
    meta.Encode(buf.data());
    (void)dm.WritePage(0, buf.data());
    (void)dm.Close();
  }

  auto storage = CreateStorage(config);
  const Status s = storage->Open(config);
  EXPECT_TRUE(s.code() == StatusCode::kVersionMismatch);

  std::filesystem::remove_all(config.data_dir);
}

// 建表/开表错误码
TEST_CASE(create_table_errors) {
  StorageConfig config = MakeConfig("errors.db");
  std::filesystem::remove_all(config.data_dir);
  {
    auto storage = CreateStorage(config);
    EXPECT_TRUE(storage->Open(config).ok());
    Schema schema;
    schema.AddColumn("id", ValueType::kInt32);
    EXPECT_TRUE(storage->create_table("t", schema).ok());
    EXPECT_TRUE(storage->create_table("t", schema).code() == StatusCode::kTableAlreadyExists);
    std::shared_ptr<TableHeap> heap;
    EXPECT_TRUE(storage->open_table("nope", &heap).code() == StatusCode::kTableNotFound);
    storage->Close();
  }
  std::filesystem::remove_all(config.data_dir);
}
