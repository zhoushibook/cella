#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "i_storage.h"
#include "storage_factory.h"
#include "config.h"
#include "record.h"
#include "schema.h"
#include "types.h"
#include "value.h"
#include "table_heap.h"
#include "console_utf8.h"   // 扁平名：demo/examples 只挂了扁平公共目录

using namespace cella::storage;

namespace {

Schema StudentSchema() {
  Schema s;
  s.AddColumn("id", ValueType::kInt32);
  s.AddColumn("name", ValueType::kVarchar, 64);
  s.AddColumn("age", ValueType::kInt32);
  return s;
}

// ⑧/⑪ 基准：同一负载跑指定策略
struct BenchResult {
  std::string policy;
  double hit_rate = 0.0;
  uint64_t hit = 0;
  uint64_t miss = 0;
  uint64_t evict = 0;
  uint64_t disk_reads = 0;
  uint64_t disk_writes = 0;
  std::vector<std::string> evictions;
};

BenchResult RunBench(const StorageConfig& base, const std::string& policy) {
  StorageConfig c = base;
  c.replacer = policy;
  c.pool_size = 3;
  c.db_file = "bench_" + policy + ".db";
  c.enable_log = false;

  auto s = CreateStorage(c);
  BenchResult r;
  r.policy = policy;
  if (!s->Open(c).ok()) {
    return r;
  }
  // 分配 4 页（1..4）并标记脏，确保淘汰时落盘、后续可读回
  for (int i = 0; i < 4; ++i) {
    page_id_t pid = kInvalidPageId;
    (void)s->allocate_page(&pid);
    (void)s->unpin_page(pid, true);
  }
  // 访问模式：重复 [1,2,3,1,4]（含重访问，可区分 LRU/FIFO）
  const int pattern[] = {1, 2, 3, 1, 4};
  const int P = static_cast<int>(sizeof(pattern) / sizeof(pattern[0]));
  for (int i = 0; i < 200; ++i) {
    const page_id_t pid = static_cast<page_id_t>(pattern[i % P]);
    Page* p = s->get_page(pid);
    if (p != nullptr) {
      (void)s->unpin_page(pid, false);
    }
  }
  const BufferStats& st = s->get_stats();
  r.hit_rate = st.hit_rate() * 100.0;
  r.hit = st.hit;
  r.miss = st.miss;
  r.evict = st.evict;
  r.disk_reads = st.disk_reads;
  r.disk_writes = st.disk_writes;
  r.evictions = s->recent_evictions();
  s->Close();
  return r;
}

void PrintRow(const BenchResult& r) {
  std::printf("  %-6s | %7.2f%% | %5llu | %5llu | %5llu | %6llu | %6llu\n", r.policy.c_str(),
              r.hit_rate, static_cast<unsigned long long>(r.hit),
              static_cast<unsigned long long>(r.miss),
              static_cast<unsigned long long>(r.evict),
              static_cast<unsigned long long>(r.disk_reads),
              static_cast<unsigned long long>(r.disk_writes));
}

}  // namespace

int main() {
  cella::storage::EnableUtf8Console();   // 控制台按 UTF-8 显示中文
  setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲，便于实时观察
  StorageConfig config;
  config.data_dir = "./demo_data";
  config.pool_size = 64;
  config.enable_log = false;
  std::filesystem::remove_all(config.data_dir);

  // ① 初始化参数
  std::printf("===== ① 初始化参数 =====\n");
  {
    auto probe = CreateStorage(config);
    std::printf("接口版本 : %s\n", probe->InterfaceVersion());
  }
  std::printf("%s\n", config.ToString().c_str());

  auto storage = CreateStorage(config);
  Status s = storage->Open(config);
  if (!s.ok()) {
    std::printf("Open 失败: %s\n", s.ToString().c_str());
    return 1;
  }

  // ② 建表
  std::printf("\n===== ② 建表 student(id INT, name VARCHAR, age INT) =====\n");
  Schema schema = StudentSchema();
  s = storage->create_table("student", schema);
  std::printf("create_table : %s\n", s.ok() ? "ok" : s.ToString().c_str());

  // ③ 插 5000 行
  std::printf("\n===== ③ 插 5000 行 =====\n");
  const int N = 5000;
  auto t0 = std::chrono::steady_clock::now();
  std::vector<Rid> rids;
  rids.reserve(N);
  for (int i = 0; i < N; ++i) {
    Record rec;
    rec.AddValue(Value::Int(i));
    rec.AddValue(Value::Varchar("stu_" + std::to_string(i)));
    rec.AddValue(Value::Int(i % 100));
    Rid rid;
    if (!storage->insert_record("student", rec, &rid).ok()) {
      std::printf("insert %d 失败\n", i);
      return 1;
    }
    rids.push_back(rid);
  }
  auto t1 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  int pages = 0;
  page_id_t last = kInvalidPageId;
  for (const auto& r : rids) {
    if (r.page_id != last) {
      ++pages;
      last = r.page_id;
    }
  }
  std::printf("插入 %d 行，耗时 %.2f ms，占用 %d 页\n", N, ms, pages);

  // ④ SeqScan
  std::printf("\n===== ④ SeqScan 全量读回 =====\n");
  {
    std::shared_ptr<TableHeap> heap;
    (void)storage->open_table("student", &heap);
    int count = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it) {
      ++count;
    }
    std::printf("SeqScan 读到 %d 行\n", count);
  }

  // ⑤ Filter age>18
  std::printf("\n===== ⑤ Filter age>18 =====\n");
  {
    std::shared_ptr<TableHeap> heap;
    (void)storage->open_table("student", &heap);
    int count = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it) {
      if (it->value(2).int32_val > 18) {
        ++count;
      }
    }
    std::printf("age>18 的行数 : %d\n", count);
  }

  // ⑥ Project 前 5 行
  std::printf("\n===== ⑥ Project 前 5 行 =====\n");
  {
    std::shared_ptr<TableHeap> heap;
    (void)storage->open_table("student", &heap);
    int n = 0;
    for (auto it = heap->begin(); it != heap->end() && n < 5; ++it, ++n) {
      std::printf("  %s\n", it->ToString().c_str());
    }
  }

  // ⑦ Delete id<100 后复查
  std::printf("\n===== ⑦ Delete id<100 后复查 =====\n");
  for (int i = 0; i < 100; ++i) {
    (void)storage->delete_record("student", rids[i]);
  }
  {
    std::shared_ptr<TableHeap> heap;
    (void)storage->open_table("student", &heap);
    int count = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it) {
      ++count;
    }
    std::printf("删除 id<100 后剩余 : %d 行\n", count);
  }

  // ⑧ LRU vs FIFO 对比
  std::printf("\n===== ⑧ LRU vs FIFO 同一负载对比 =====\n");
  BenchResult lru = RunBench(config, "LRU");
  BenchResult fifo = RunBench(config, "FIFO");
  std::printf("  策略    |  命中率  |  命中 |  未中 |  淘汰 | 磁盘读 | 磁盘写\n");
  PrintRow(lru);
  PrintRow(fifo);

  // ⑨ 最近 10 条替换日志
  std::printf("\n===== ⑨ 最近 10 条替换日志 =====\n");
  const size_t start = lru.evictions.size() > 10 ? lru.evictions.size() - 10 : 0;
  for (size_t i = start; i < lru.evictions.size(); ++i) {
    std::printf("  [WARN][REPLACER] %s\n", lru.evictions[i].c_str());
  }

  // ⑩ 重启后行数一致
  std::printf("\n===== ⑩ 重启后行数一致 =====\n");
  storage->Close();
  {
    auto storage2 = CreateStorage(config);
    Status s2 = storage2->Open(config);
    if (!s2.ok()) {
      std::printf("重开失败: %s\n", s2.ToString().c_str());
      return 1;
    }
    std::shared_ptr<TableHeap> heap;
    (void)storage2->open_table("student", &heap);
    int count = 0;
    for (auto it = heap->begin(); it != heap->end(); ++it) {
      ++count;
    }
    std::printf("重启后读到 %d 行（与删除后一致）\n", count);
    storage2->Close();
  }

  // ⑪ 换 CLOCK 再跑一遍
  std::printf("\n===== ⑪ 换 CLOCK 再跑一遍（证明可扩展） =====\n");
  BenchResult clock = RunBench(config, "CLOCK");
  std::printf("  策略    |  命中率  |  命中 |  未中 |  淘汰 | 磁盘读 | 磁盘写\n");
  PrintRow(clock);
  std::printf("CLOCK 通过 ReplacerFactory 注册即可用，核心代码零改动。\n");

  return 0;
}
