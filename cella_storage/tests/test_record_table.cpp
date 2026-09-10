#include "mini_test.h"

#include <memory>
#include <string>
#include <vector>

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/buffer/replacer_factory.h"
#include "cella/storage/common/record.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/status.h"
#include "cella/storage/common/value.h"
#include "cella/storage/disk/mem_disk_manager.h"
#include "cella/storage/page/page.h"
#include "cella/storage/record/slotted_record_serializer.h"
#include "cella/storage/table/table_heap.h"

using namespace cella::storage;

namespace {

std::unique_ptr<BufferPoolManager> MakeBpm(size_t pool) {
  auto disk = std::make_unique<MemDiskManager>(4096);
  (void)disk->Open("");
  auto replacer = ReplacerFactory::Create("LRU", pool);
  return std::make_unique<BufferPoolManager>(pool, std::move(disk), std::move(replacer));
}

// 分配并初始化首数据页，构造 TableHeap
std::unique_ptr<TableHeap> MakeTableHeap(BufferPoolManager* bpm, const Schema& schema,
                                         IRecordSerializer* ser) {
  page_id_t first = kInvalidPageId;
  Page* p = bpm->NewPage(&first);
  if (p == nullptr) {
    return nullptr;
  }
  {
    PageGuard pg(bpm, p);
    pg->SetHeaderPageId(first);
    pg->SetHeaderPageType(PageType::kDataPage);
    pg->SetHeaderPrevPageId(kInvalidPageId);
    pg->SetHeaderNextPageId(kInvalidPageId);
    pg->SetSlotCount(0);
    pg->SetFreeEnd(static_cast<uint16_t>(pg->page_size()));
    pg.MarkDirty();
  }
  return std::make_unique<TableHeap>(bpm, first, schema, ser);
}

}  // namespace

// INT/VARCHAR/NULL 序列化往返
TEST_CASE(serialize_roundtrip_int_varchar_null) {
  Schema schema;
  schema.AddColumn("id", ValueType::kInt32);
  schema.AddColumn("name", ValueType::kVarchar, 64);
  schema.AddColumn("age", ValueType::kInt32);
  schema.AddColumn("note", ValueType::kVarchar, 128);

  SlottedRecordSerializer ser;
  Record rec;
  rec.AddValue(Value::Int(42));
  rec.AddValue(Value::Varchar("张三"));
  rec.AddValue(Value::Null());
  rec.AddValue(Value::Varchar("hello"));

  std::vector<char> bytes;
  EXPECT_TRUE(ser.Serialize(rec, schema, &bytes).ok());

  Record out;
  EXPECT_TRUE(ser.Deserialize(bytes.data(), bytes.size(), schema, &out).ok());
  EXPECT_EQ(out.value_count(), size_t(4));
  EXPECT_EQ(out.value(0).int32_val, 42);
  EXPECT_TRUE(out.value(1).str_val == "张三");
  EXPECT_TRUE(out.value(2).IsNull());
  EXPECT_TRUE(out.value(3).str_val == "hello");
}

// 插 1000 行，全表扫描一致（跨页扩展）
TEST_CASE(insert_1000_rows_scan) {
  auto bpm = MakeBpm(64);
  Schema schema;
  schema.AddColumn("id", ValueType::kInt32);
  schema.AddColumn("name", ValueType::kVarchar, 64);
  schema.AddColumn("age", ValueType::kInt32);

  SlottedRecordSerializer ser;
  auto heap = MakeTableHeap(bpm.get(), schema, &ser);
  EXPECT_TRUE(heap != nullptr);

  const int N = 1000;
  for (int i = 0; i < N; ++i) {
    Record rec;
    rec.AddValue(Value::Int(i));
    rec.AddValue(Value::Varchar("name_" + std::to_string(i)));
    rec.AddValue(Value::Int(i % 100));
    Rid rid;
    EXPECT_TRUE(heap->InsertRecord(rec, &rid).ok());
  }

  int count = 0;
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    const Record& r = *it;
    const int id = r.value(0).int32_val;
    EXPECT_EQ(id, count);   // 按插入顺序扫描
    EXPECT_TRUE(r.value(1).str_val == "name_" + std::to_string(id));
    EXPECT_EQ(r.value(2).int32_val, id % 100);
    ++count;
  }
  EXPECT_EQ(count, N);
}

// 删除后扫描跳过墓碑
TEST_CASE(delete_then_scan_skips) {
  auto bpm = MakeBpm(64);
  Schema schema;
  schema.AddColumn("id", ValueType::kInt32);
  schema.AddColumn("name", ValueType::kVarchar, 64);

  SlottedRecordSerializer ser;
  auto heap = MakeTableHeap(bpm.get(), schema, &ser);

  const int N = 100;
  std::vector<Rid> rids;
  for (int i = 0; i < N; ++i) {
    Record rec;
    rec.AddValue(Value::Int(i));
    rec.AddValue(Value::Varchar("n" + std::to_string(i)));
    Rid rid;
    EXPECT_TRUE(heap->InsertRecord(rec, &rid).ok());
    rids.push_back(rid);
  }

  int deleted = 0;
  for (int i = 0; i < N; i += 2) {   // 删除偶数 id
    EXPECT_TRUE(heap->DeleteRecord(rids[i]).ok());
    ++deleted;
  }

  int count = 0;
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    const int id = it->value(0).int32_val;
    EXPECT_TRUE(id % 2 == 1);   // 只剩奇数
    ++count;
  }
  EXPECT_EQ(count, N - deleted);
}

// 超长记录返回 kRecordTooLarge
TEST_CASE(record_too_large) {
  auto bpm = MakeBpm(8);
  Schema schema;
  schema.AddColumn("data", ValueType::kVarchar, 5000);

  SlottedRecordSerializer ser;
  auto heap = MakeTableHeap(bpm.get(), schema, &ser);

  Record rec;
  rec.AddValue(Value::Varchar(std::string(5000, 'x')));
  Rid rid;
  const Status s = heap->InsertRecord(rec, &rid);
  EXPECT_TRUE(s.code() == StatusCode::kRecordTooLarge);
}
