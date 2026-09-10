#include "mini_test.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cella/storage/common/types.h"
#include "cella/storage/disk/file_disk_manager.h"
#include "cella/storage/disk/mem_disk_manager.h"
#include "cella/storage/page/page.h"
#include "cella/storage/page/slotted_page_layout.h"

using namespace cella::storage;

namespace {
const char* kTmpFile = "phase1_tmp.db";
}  // namespace

// 页偏移公式 + 写后重开字节一致
TEST_CASE(disk_offset_and_reopen) {
  std::remove(kTmpFile);
  {
    FileDiskManager dm(512);
    EXPECT_TRUE(dm.Open(kTmpFile).ok());
    const page_id_t p1 = dm.AllocatePage();
    const page_id_t p2 = dm.AllocatePage();
    const page_id_t p3 = dm.AllocatePage();
    EXPECT_EQ(p1, 1u);
    EXPECT_EQ(p2, 2u);
    EXPECT_EQ(p3, 3u);

    std::vector<char> a(512, 'A'), b(512, 'B'), c(512, 'C');
    EXPECT_TRUE(dm.WritePage(p1, a.data()).ok());
    EXPECT_TRUE(dm.WritePage(p2, b.data()).ok());
    EXPECT_TRUE(dm.WritePage(p3, c.data()).ok());
    EXPECT_TRUE(dm.Close().ok());
  }
  {
    // 重开，字节一致
    FileDiskManager dm(512);
    EXPECT_TRUE(dm.Open(kTmpFile).ok());
    std::vector<char> buf(512, 0);
    EXPECT_TRUE(dm.ReadPage(2, buf.data()).ok());
    EXPECT_TRUE(buf[0] == 'B' && buf[511] == 'B');
    EXPECT_TRUE(dm.ReadPage(3, buf.data()).ok());
    EXPECT_TRUE(buf[0] == 'C' && buf[511] == 'C');
    EXPECT_TRUE(dm.Close().ok());
  }
  std::remove(kTmpFile);
}

// 偏移公式：写 page 5（跳过 1~4），文件应扩展到 6 * 512
TEST_CASE(disk_offset_formula) {
  std::remove(kTmpFile);
  {
    FileDiskManager dm(512);
    EXPECT_TRUE(dm.Open(kTmpFile).ok());
    std::vector<char> x(512, 'X');
    EXPECT_TRUE(dm.WritePage(5, x.data()).ok());
    EXPECT_TRUE(dm.Close().ok());
  }
  std::ifstream f(kTmpFile, std::ios::binary | std::ios::ate);
  EXPECT_EQ(static_cast<size_t>(f.tellg()), size_t(6 * 512));
  f.close();
  std::remove(kTmpFile);
}

// 释放后再分配复用同一页号（文件后端）
TEST_CASE(disk_free_list_reuse) {
  std::remove(kTmpFile);
  FileDiskManager dm(512);
  EXPECT_TRUE(dm.Open(kTmpFile).ok());
  const page_id_t p1 = dm.AllocatePage();
  const page_id_t p2 = dm.AllocatePage();
  dm.AllocatePage();
  EXPECT_TRUE(dm.FreePage(p2).ok());
  const uint64_t count_before = dm.GetPageCount();
  const page_id_t reuse = dm.AllocatePage();
  EXPECT_EQ(reuse, p2);                      // 复用 p2
  EXPECT_EQ(dm.GetPageCount(), count_before); // 页数不增长
  EXPECT_TRUE(dm.FreePage(p1).ok());
  EXPECT_EQ(dm.AllocatePage(), p1);           // 复用 p1
  EXPECT_TRUE(dm.Close().ok());
  std::remove(kTmpFile);
}

// 内存后端同样支持释放复用
TEST_CASE(mem_disk_free_list_reuse) {
  MemDiskManager dm(512);
  EXPECT_TRUE(dm.Open("").ok());
  const page_id_t p1 = dm.AllocatePage();
  const page_id_t p2 = dm.AllocatePage();
  EXPECT_TRUE(dm.FreePage(p1).ok());
  EXPECT_EQ(dm.AllocatePage(), p1);   // 复用 p1
  EXPECT_EQ(dm.GetPageCount(), uint64_t(3));  // meta + p1 + p2
  EXPECT_EQ(p2, 2u);
}

// 槽式布局：插入 / 读取 / 删除（标记删除）
TEST_CASE(slotted_insert_get_delete) {
  Page page(512);
  SlottedPageLayout layout(&page);
  slot_id_t s0 = 0, s1 = 0;
  EXPECT_TRUE(layout.InsertRecord("hello", 5, &s0).ok());
  EXPECT_TRUE(layout.InsertRecord("world", 5, &s1).ok());
  EXPECT_EQ(s0, 0u);
  EXPECT_EQ(s1, 1u);

  const char* data = nullptr;
  uint16_t len = 0;
  EXPECT_TRUE(layout.GetRecord(s0, &data, &len).ok());
  EXPECT_EQ(len, 5u);
  EXPECT_TRUE(std::memcmp(data, "hello", 5) == 0);

  // 删除 s0 → 标记删除
  EXPECT_TRUE(layout.DeleteRecord(s0).ok());
  EXPECT_TRUE(layout.GetRecord(s0, &data, &len).code() == StatusCode::kInvalidArgument);
  // s1 仍可读
  EXPECT_TRUE(layout.GetRecord(s1, &data, &len).ok());
  EXPECT_TRUE(std::memcmp(data, "world", 5) == 0);
}

// 页满 kPageFull + 记录超长 kRecordTooLarge
TEST_CASE(slotted_page_full_and_too_large) {
  Page page(512);
  SlottedPageLayout layout(&page);
  char rec[64];
  std::memset(rec, 'x', 64);
  slot_id_t slot = 0;
  int inserted = 0;
  while (layout.InsertRecord(rec, 64, &slot).ok()) {
    ++inserted;
  }
  EXPECT_EQ(inserted, 7);   // (512-32)/68 = 7 条

  const Status s = layout.InsertRecord(rec, 64, &slot);
  EXPECT_TRUE(s.code() == StatusCode::kPageFull);

  // 记录超长（> 512-32-4 = 476）
  std::vector<char> big(512, 'y');
  Page page2(512);
  SlottedPageLayout layout2(&page2);
  slot_id_t slot2 = 0;
  const Status s2 = layout2.InsertRecord(big.data(), 512, &slot2);
  EXPECT_TRUE(s2.code() == StatusCode::kRecordTooLarge);
}
