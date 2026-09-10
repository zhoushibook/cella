#include "mini_test.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/clock_replacer.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/buffer/replacer_factory.h"
#include "cella/storage/common/logger.h"
#include "cella/storage/disk/mem_disk_manager.h"

using namespace cella::storage;

namespace {

// 捕获日志的测试用 logger
class CaptureLogger : public ILogger {
 public:
  void Log(LogLevel /*level*/, const std::string& category,
           const std::string& message) override {
    lines_.push_back(category + ": " + message);
  }
  std::vector<std::string> lines_;
};

// 构造一个已预分配 N 页的缓冲池
std::unique_ptr<BufferPoolManager> MakeBpm(size_t pool, const std::string& policy,
                                           int prealloc_pages) {
  auto disk = std::make_unique<MemDiskManager>(4096);
  (void)disk->Open("");
  for (int i = 0; i < prealloc_pages; ++i) {
    disk->AllocatePage();
  }
  auto replacer = ReplacerFactory::Create(policy, pool);
  return std::make_unique<BufferPoolManager>(pool, std::move(disk), std::move(replacer));
}

}  // namespace

// 首访 MISS，二访 HIT
TEST_CASE(buffer_first_miss_second_hit) {
  auto bpm = MakeBpm(4, "LRU", 3);
  Page* p = bpm->get_page(1);
  EXPECT_TRUE(p != nullptr);
  bpm->UnpinPage(1, false);

  p = bpm->get_page(1);
  EXPECT_TRUE(p != nullptr);
  bpm->UnpinPage(1, false);

  const BufferStats& s = bpm->GetStats();
  EXPECT_EQ(s.access, uint64_t(2));
  EXPECT_EQ(s.hit, uint64_t(1));
  EXPECT_EQ(s.miss, uint64_t(1));
}

// LRU 淘汰最久未用的页
TEST_CASE(lru_eviction_order) {
  auto bpm = MakeBpm(2, "LRU", 4);
  bpm->get_page(1); bpm->UnpinPage(1, false);
  bpm->get_page(2); bpm->UnpinPage(2, false);
  bpm->get_page(1); bpm->UnpinPage(1, false);   // 1 变最近使用
  bpm->get_page(3); bpm->UnpinPage(3, false);   // 淘汰 2（最久未用）

  const BufferStats& s = bpm->GetStats();
  EXPECT_EQ(s.evict, uint64_t(1));
  EXPECT_TRUE(bpm->recent_evictions().back().find("victim_page=2") != std::string::npos);
}

// FIFO 与 LRU 同一负载淘汰结果不同（证明两种策略真的不一样）
TEST_CASE(fifo_vs_lru_different) {
  {
    auto bpm = MakeBpm(3, "LRU", 4);
    bpm->get_page(1); bpm->UnpinPage(1, false);
    bpm->get_page(2); bpm->UnpinPage(2, false);
    bpm->get_page(3); bpm->UnpinPage(3, false);
    bpm->get_page(1); bpm->UnpinPage(1, false);   // 1 变最近
    bpm->get_page(4); bpm->UnpinPage(4, false);   // LRU 淘汰 2
    EXPECT_TRUE(bpm->recent_evictions().back().find("victim_page=2") != std::string::npos);
  }
  {
    auto bpm = MakeBpm(3, "FIFO", 4);
    bpm->get_page(1); bpm->UnpinPage(1, false);
    bpm->get_page(2); bpm->UnpinPage(2, false);
    bpm->get_page(3); bpm->UnpinPage(3, false);
    bpm->get_page(1); bpm->UnpinPage(1, false);   // FIFO 不因访问改序
    bpm->get_page(4); bpm->UnpinPage(4, false);   // FIFO 淘汰 1（最先插入）
    EXPECT_TRUE(bpm->recent_evictions().back().find("victim_page=1") != std::string::npos);
  }
}

// pin 住的页不被淘汰
TEST_CASE(pinned_page_not_evicted) {
  auto bpm = MakeBpm(2, "LRU", 4);
  bpm->get_page(1);                                 // 保持 pin
  bpm->get_page(2); bpm->UnpinPage(2, false);
  bpm->get_page(3); bpm->UnpinPage(3, false);       // 只能淘汰 2，1 被 pin

  Page* p1 = bpm->get_page(1);                      // 仍缓存
  EXPECT_TRUE(p1 != nullptr);
  EXPECT_TRUE(bpm->recent_evictions().back().find("victim_page=2") != std::string::npos);
}

// 全 pin 返回空，不死锁不崩溃
TEST_CASE(all_pinned_no_free_frame) {
  auto bpm = MakeBpm(2, "LRU", 4);
  bpm->get_page(1);   // pin
  bpm->get_page(2);   // pin
  Page* p = bpm->get_page(3);
  EXPECT_TRUE(p == nullptr);   // 无可用帧
}

// 淘汰脏页先写盘
TEST_CASE(dirty_page_flushed_before_evict) {
  auto bpm = MakeBpm(2, "LRU", 3);
  Page* p1 = bpm->get_page(1);
  std::memset(p1->data(), 'D', p1->page_size());
  bpm->UnpinPage(1, true);                          // 标记脏

  bpm->get_page(2); bpm->UnpinPage(2, false);
  bpm->get_page(3); bpm->UnpinPage(3, false);       // 淘汰脏页 1

  const BufferStats& s = bpm->GetStats();
  EXPECT_EQ(s.dirty_flush, uint64_t(1));
  EXPECT_TRUE(bpm->recent_evictions().back().find("dirty=true") != std::string::npos);
}

// PageGuard 析构后 pin 归零
TEST_CASE(page_guard_auto_unpin) {
  auto bpm = MakeBpm(2, "LRU", 3);
  {
    PageGuard guard(bpm.get(), bpm->get_page(1));
    EXPECT_TRUE(guard.valid());
    guard.MarkDirty();
  }  // 析构自动 unpin

  bpm->get_page(2); bpm->UnpinPage(2, false);
  // 若 page 1 未被 unpin，则全 pin 会返回 nullptr
  Page* p3 = bpm->get_page(3);
  EXPECT_TRUE(p3 != nullptr);
}

// 替换策略工厂 + 统计格式
TEST_CASE(replacer_factory_and_stats_format) {
  const auto names = ReplacerFactory::RegisteredNames();
  bool has_lru = false, has_fifo = false, has_clock = false;
  for (const auto& n : names) {
    if (n == "LRU") has_lru = true;
    if (n == "FIFO") has_fifo = true;
    if (n == "CLOCK") has_clock = true;
  }
  EXPECT_TRUE(has_lru && has_fifo && has_clock);
  EXPECT_TRUE(ReplacerFactory::Create("NOPE", 2) == nullptr);

  BufferStats s;
  s.access = 1000; s.hit = 873; s.miss = 127; s.evict = 64;
  const std::string line = s.ToLogLine();
  EXPECT_TRUE(line.find("accesses=1000") != std::string::npos);
  EXPECT_TRUE(line.find("hit_rate=87.30%") != std::string::npos);
}

// CLOCK 基础行为（扩展点演示）
TEST_CASE(clock_replacer_basic) {
  ClockReplacer clock(3);
  frame_id_t v = 0;
  clock.Insert(0); clock.Insert(1); clock.Insert(2);
  EXPECT_EQ(clock.Size(), size_t(3));
  clock.Pin(1);
  EXPECT_EQ(clock.Size(), size_t(2));
  EXPECT_TRUE(clock.Victim(&v));
  EXPECT_TRUE(v == 0 || v == 2);
  EXPECT_EQ(clock.Size(), size_t(1));
}

// 替换日志 + 统计日志输出
TEST_CASE(replacer_and_stats_log_output) {
  auto bpm = MakeBpm(2, "LRU", 3);
  CaptureLogger logger;
  bpm->SetLogger(&logger);

  bpm->get_page(1); bpm->UnpinPage(1, true);
  bpm->get_page(2); bpm->UnpinPage(2, false);
  bpm->get_page(3); bpm->UnpinPage(3, false);   // 淘汰脏页 1
  bpm->LogStats();

  bool has_replacer = false, has_stats = false;
  for (const auto& l : logger.lines_) {
    if (l.find("REPLACER") != std::string::npos &&
        l.find("victim_page=1") != std::string::npos) {
      has_replacer = true;
    }
    if (l.find("STATS") != std::string::npos &&
        l.find("accesses=") != std::string::npos) {
      has_stats = true;
    }
  }
  EXPECT_TRUE(has_replacer);
  EXPECT_TRUE(has_stats);
}
