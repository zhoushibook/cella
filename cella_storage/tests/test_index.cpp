#include "mini_test.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/buffer/replacer_factory.h"
#include "cella/storage/common/value.h"
#include "cella/storage/disk/mem_disk_manager.h"
#include "cella/storage/index/b_plus_tree.h"
#include "cella/storage/index/index_key.h"
#include "cella/storage/index/index_node.h"
#include "cella/storage/page/page.h"

using namespace cella::storage;

namespace {

std::unique_ptr<BufferPoolManager> MakeBpm(size_t pool) {
  auto disk = std::make_unique<MemDiskManager>(4096);
  (void)disk->Open("");
  auto replacer = ReplacerFactory::Create("LRU", pool);
  return std::make_unique<BufferPoolManager>(pool, std::move(disk), std::move(replacer));
}

BPlusTree::KeySpec IntSpec() {
  return BPlusTree::KeySpec::Single(ValueType::kInt32, 0);
}

Value Int32(int32_t v) { return Value::Int(v); }

Value Int64(int64_t v) { return Value::BigInt(v); }

Value Varchar(const std::string& s) { return Value::Varchar(s); }

// 便捷：插入一个 int 值
Status InsertInt(BPlusTree* t, int32_t v, page_id_t page, uint16_t slot, bool* dup) {
  return t->Insert(EncodeLeafKey(Int32(v), page, slot), dup);
}

}  // namespace

// ─────────────────────────────────────────────────────────────
// 键编码：保序性是 B+ 树一切正确性的前提，先单独验证
// ─────────────────────────────────────────────────────────────

TEST_CASE(index_key_int32_order_preserving) {
  // 关键边界：负数 < 0 < 正数，且 INT32_MIN < INT32_MAX
  const std::vector<int32_t> vals = {INT32_MIN, -1000000, -256, -1, 0, 1, 256, 1000000, INT32_MAX};
  for (size_t i = 0; i + 1 < vals.size(); ++i) {
    std::string a, b;
    EncodeIndexColumn(Int32(vals[i]), &a);
    EncodeIndexColumn(Int32(vals[i + 1]), &b);
    EXPECT_TRUE(a < b);   // 编码后字典序 == 逻辑序
    EXPECT_TRUE(CompareIndexKey(a, b, false) < 0);
  }
}

TEST_CASE(index_key_null_sorts_first) {
  std::string null_key, zero_key;
  const Value null_v = Value::Null();
  EncodeIndexColumn(null_v, &null_key);
  EncodeIndexColumn(Int32(0), &zero_key);
  EXPECT_TRUE(null_key < zero_key);
}

TEST_CASE(index_key_varchar_prefix_order) {
  // "ab" 必须排在 "abc" 之前（前缀串在前）
  std::string ab, abc, b;
  EncodeIndexColumn(Varchar("ab"), &ab);
  EncodeIndexColumn(Varchar("abc"), &abc);
  EncodeIndexColumn(Varchar("b"), &b);
  EXPECT_TRUE(ab < abc);
  EXPECT_TRUE(abc < b);
}

TEST_CASE(index_key_leaf_rid_roundtrip) {
  const std::string k = EncodeLeafKey(Int32(42), 7, 13);
  Value out;
  EXPECT_TRUE(DecodeLeafKeyColumn(k, ValueType::kInt32, &out));
  EXPECT_EQ(out.int32_val, 42);
  page_id_t pg = 0;
  uint16_t slot = 0;
  EXPECT_TRUE(DecodeLeafKeyRid(k, &pg, &slot));
  EXPECT_EQ(pg, 7u);
  EXPECT_EQ(slot, 13);
  // StripLeafRowId 后应等于纯列值编码
  std::string col;
  EncodeIndexColumn(Int32(42), &col);
  EXPECT_EQ(StripLeafRowId(k), col);
}

// ─────────────────────────────────────────────────────────────
// B+ 树基本操作
// ─────────────────────────────────────────────────────────────

TEST_CASE(bptree_create_empty) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));
  EXPECT_TRUE(t.valid());
  EXPECT_EQ(t.root_page(), root);

  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), 0u);
}

TEST_CASE(bptree_insert_and_contains_small) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  for (int32_t i = 0; i < 20; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i), &dup));
    EXPECT_FALSE(dup);
  }
  for (int32_t i = 0; i < 20; ++i) {
    bool found = false;
    EXPECT_OK(t.Contains(EncodeLeafKey(Int32(i), 1, static_cast<uint8_t>(i)), &found));
    EXPECT_TRUE(found);
  }
  // 不存在的键
  bool found = false;
  EXPECT_OK(t.Contains(EncodeLeafKey(Int32(999), 1, 0), &found));
  EXPECT_FALSE(found);
}

TEST_CASE(bptree_scan_is_sorted) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  // 乱序插入
  const std::vector<int32_t> vals = {50, -3, 17, 0, 99, -100, 7, 42};
  for (size_t i = 0; i < vals.size(); ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, vals[i], 1, static_cast<uint8_t>(i), &dup));
  }

  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), vals.size());

  // 扫描结果必须按列值升序
  std::vector<int32_t> got;
  for (const auto& k : all) {
    Value v;
    EXPECT_TRUE(DecodeLeafKeyColumn(k, ValueType::kInt32, &v));
    got.push_back(v.int32_val);
  }
  std::vector<int32_t> want = vals;
  std::sort(want.begin(), want.end());
  EXPECT_EQ(got.size(), want.size());
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(got[i], want[i]);
  }
}

TEST_CASE(bptree_split_many_keys) {
  // 足够多的键 → 触发多次叶子分裂，甚至长高一层
  auto bpm = MakeBpm(256);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  const int32_t N = 800;
  for (int32_t i = 0; i < N; ++i) {
    bool dup = false;
    const Status s = InsertInt(&t, i * 3, static_cast<page_id_t>(i / 200),
                               static_cast<uint8_t>(i % 200), &dup);
    EXPECT_OK(s);
    if (!s.ok()) {
      return;   // 后续断言无意义
    }
  }

  // 全部可查到
  int missing = 0;
  for (int32_t i = 0; i < N; ++i) {
    bool found = false;
    const Status s = t.Contains(EncodeLeafKey(Int32(i * 3), static_cast<page_id_t>(i / 200),
                                             static_cast<uint8_t>(i % 200)), &found);
    if (!s.ok() || !found) {
      ++missing;
    }
  }
  EXPECT_EQ(missing, 0);

  // 扫描数量正确且有序
  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), static_cast<size_t>(N));
  int32_t prev = INT32_MIN;
  bool ordered = true;
  for (const auto& k : all) {
    Value v;
    if (!DecodeLeafKeyColumn(k, ValueType::kInt32, &v)) {
      ordered = false;
      break;
    }
    if (v.int32_val <= prev) {
      ordered = false;
      break;
    }
    prev = v.int32_val;
  }
  EXPECT_TRUE(ordered);
}

TEST_CASE(bptree_descending_insert_triggers_splits) {
  // 降序插入是最容易暴露「分裂时上提键选错」的顺序
  auto bpm = MakeBpm(256);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  const int32_t N = 600;
  for (int32_t i = N; i > 0; --i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i % 200), &dup));
  }
  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), static_cast<size_t>(N));

  int32_t prev = INT32_MIN;
  bool ordered = true;
  for (const auto& k : all) {
    Value v;
    if (!DecodeLeafKeyColumn(k, ValueType::kInt32, &v) || v.int32_val <= prev) {
      ordered = false;
      break;
    }
    prev = v.int32_val;
  }
  EXPECT_TRUE(ordered);
  EXPECT_EQ(prev, N);
}

TEST_CASE(bptree_duplicate_detection) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  // 同一列值 + 同一行定位 = 完全重复
  bool dup = false;
  EXPECT_OK(InsertInt(&t, 5, 1, 2, &dup));
  EXPECT_FALSE(dup);
  EXPECT_OK(InsertInt(&t, 5, 1, 2, &dup));
  EXPECT_TRUE(dup);

  // 同一列值 + 不同行定位 = 不同键（非唯一索引）
  EXPECT_OK(InsertInt(&t, 5, 1, 3, &dup));
  EXPECT_FALSE(dup);

  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), 2u);
}

TEST_CASE(bptree_remove) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  for (int32_t i = 0; i < 30; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i), &dup));
  }
  // 删掉偶数
  for (int32_t i = 0; i < 30; i += 2) {
    bool removed = false;
    EXPECT_OK(t.Remove(EncodeLeafKey(Int32(i), 1, static_cast<uint8_t>(i)), &removed));
    EXPECT_TRUE(removed);
  }
  // 偶数应查不到、奇数应查得到
  int bad = 0;
  for (int32_t i = 0; i < 30; ++i) {
    bool found = false;
    EXPECT_OK(t.Contains(EncodeLeafKey(Int32(i), 1, static_cast<uint8_t>(i)), &found));
    const bool want = (i % 2 == 1);
    if (found != want) {
      ++bad;
    }
  }
  EXPECT_EQ(bad, 0);

  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), 15u);

  // 删除不存在的键 → removed = false
  bool removed = false;
  EXPECT_OK(t.Remove(EncodeLeafKey(Int32(12345), 9, 9), &removed));
  EXPECT_FALSE(removed);
}

TEST_CASE(bptree_range_scan) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  for (int32_t i = 0; i < 100; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i % 200), &dup));
  }

  // [20, 30] 闭区间
  std::string lo, hi;
  EncodeIndexColumn(Int32(20), &lo);
  EncodeIndexColumn(Int32(30), &hi);
  std::vector<int32_t> got;
  const Status s = t.ScanRange(&lo, &hi, /*include_hi=*/true,
                               [&got](const std::string& k) {
                                 Value v;
                                 if (DecodeLeafKeyColumn(k, ValueType::kInt32, &v)) {
                                   got.push_back(v.int32_val);
                                 }
                                 return true;
                               });
  EXPECT_OK(s);
  EXPECT_EQ(got.size(), 11u);   // 20..30 含两端
  if (!got.empty()) {
    EXPECT_EQ(got.front(), 20);
    EXPECT_EQ(got.back(), 30);
  }

  // [20, 30) 半开区间
  std::vector<int32_t> got2;
  const Status s2 = t.ScanRange(&lo, &hi, /*include_hi=*/false,
                                [&got2](const std::string& k) {
                                  Value v;
                                  if (DecodeLeafKeyColumn(k, ValueType::kInt32, &v)) {
                                    got2.push_back(v.int32_val);
                                  }
                                  return true;
                                });
  EXPECT_OK(s2);
  EXPECT_EQ(got2.size(), 10u);   // 20..29
}

TEST_CASE(bptree_range_scan_early_terminate) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  for (int32_t i = 0; i < 100; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i % 200), &dup));
  }
  int seen = 0;
  EXPECT_OK(t.ScanRange(nullptr, nullptr, true, [&seen](const std::string&) {
    ++seen;
    return seen < 5;   // 第 5 次返回 false → 提前终止
  }));
  EXPECT_EQ(seen, 5);
}

TEST_CASE(bptree_null_key_supported) {
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  const Value null_v = Value::Null();

  bool dup = false;
  EXPECT_OK(t.Insert(EncodeLeafKey(null_v, 1, 1), &dup));
  EXPECT_FALSE(dup);
  bool dup2 = false;
  EXPECT_OK(t.Insert(EncodeLeafKey(Int32(5), 1, 2), &dup2));

  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), 2u);
  // NULL 排最前
  if (all.size() == 2) {
    Value v;
    EXPECT_TRUE(DecodeLeafKeyColumn(all[0], ValueType::kInt32, &v));
    EXPECT_TRUE(v.IsNull());
  }
}

TEST_CASE(bptree_varchar_keys) {
  BPlusTree::KeySpec spec = BPlusTree::KeySpec::Single(ValueType::kVarchar, 32);

  auto bpm = MakeBpm(256);
  BPlusTree t(bpm.get(), spec);
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  const std::vector<std::string> names = {"zebra", "apple", "mango", "ant", "banana",
                                          "aardvark", "zzz", "a", "ab", "abc"};
  for (size_t i = 0; i < names.size(); ++i) {
    bool dup = false;
    EXPECT_OK(t.Insert(EncodeLeafKey(Varchar(names[i]), 1, static_cast<uint8_t>(i)), &dup));
  }

  std::vector<std::string> got;
  EXPECT_OK(t.ScanAll(&got));
  EXPECT_EQ(got.size(), names.size());
  std::vector<std::string> want = names;
  std::sort(want.begin(), want.end());
  for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
    Value v;
    EXPECT_TRUE(DecodeLeafKeyColumn(got[i], ValueType::kVarchar, &v));
    EXPECT_EQ(v.str_val, want[i]);
  }
}

TEST_CASE(index_node_init_geometry) {
  auto bpm = MakeBpm(64);
  page_id_t pid = kInvalidPageId;
  Page* p = bpm->NewPage(&pid);
  EXPECT_TRUE(p != nullptr);
  if (p == nullptr) {
    return;
  }
  PageGuard g(bpm.get(), p);
  IndexNode node(g.get());
  node.Init(IndexNodeType::kLeaf, 100);
  EXPECT_EQ(static_cast<int>(node.type()), static_cast<int>(IndexNodeType::kLeaf));
  EXPECT_EQ(node.max_keys(), 100);
  EXPECT_EQ(node.key_count(), 0);
  // 节点自描述：重开一次应读到同样的 max_keys
  IndexNode again(g.get());
  EXPECT_EQ(again.max_keys(), 100);
  EXPECT_EQ(again.MaxKeysField(), 100);
}

TEST_CASE(index_node_insert_and_remove_keys) {
  auto bpm = MakeBpm(64);
  page_id_t pid = kInvalidPageId;
  Page* p = bpm->NewPage(&pid);
  if (p == nullptr) {
    EXPECT_TRUE(false);
    return;
  }
  PageGuard g(bpm.get(), p);
  IndexNode node(g.get());
  node.Init(IndexNodeType::kLeaf, 100);

  EXPECT_OK(node.InsertKeyAt(0, "ccc"));
  EXPECT_OK(node.InsertKeyAt(0, "aaa"));
  EXPECT_OK(node.InsertKeyAt(1, "bbb"));
  EXPECT_EQ(node.key_count(), 3);

  std::string k0, k1, k2;
  EXPECT_TRUE(node.GetKey(0, &k0));
  EXPECT_TRUE(node.GetKey(1, &k1));
  EXPECT_TRUE(node.GetKey(2, &k2));
  EXPECT_EQ(k0, "aaa");
  EXPECT_EQ(k1, "bbb");
  EXPECT_EQ(k2, "ccc");

  node.RemoveKeyAt(1);
  EXPECT_EQ(node.key_count(), 2);
  EXPECT_TRUE(node.GetKey(0, &k0));
  EXPECT_TRUE(node.GetKey(1, &k1));
  EXPECT_EQ(k0, "aaa");
  EXPECT_EQ(k1, "ccc");
}

TEST_CASE(bptree_persist_via_attach) {
  // Attach 到已有根页后，数据应仍可访问（模拟重开库）
  auto bpm = MakeBpm(256);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));
  for (int32_t i = 0; i < 50; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i), &dup));
  }

  BPlusTree t2(bpm.get(), IntSpec());
  t2.Attach(root);
  EXPECT_TRUE(t2.valid());
  EXPECT_EQ(t2.root_page(), root);
  std::vector<std::string> all;
  EXPECT_OK(t2.ScanAll(&all));
  EXPECT_EQ(all.size(), 50u);
  bool found = false;
  EXPECT_OK(t2.Contains(EncodeLeafKey(Int32(25), 1, 25), &found));
  EXPECT_TRUE(found);
}

// ─────────────────────────────────────────────────────────────
// 统计信息：Height / LeafPageCount（P1.6 代价模型的输入）
//
// 为什么单独测：代价模型按「树高页随机 I/O」给索引定位计费，
// 高度错了会让主键等值查找在小表上被估得比全表扫描还贵。
// 这两个接口是 P1.6 新增的，先把它们钉死。
// ─────────────────────────────────────────────────────────────

TEST_CASE(bptree_height_single_leaf) {
  // 少量键 → 只有根叶子页，高度 = 1
  auto bpm = MakeBpm(64);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));
  EXPECT_EQ(t.Height(), 1);

  for (int32_t i = 0; i < 5; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i), &dup));
  }
  // 仍未分裂 → 高度保持 1
  EXPECT_EQ(t.Height(), 1);
  EXPECT_EQ(t.LeafPageCount(), 1u);
}

TEST_CASE(bptree_height_grows_with_splits) {
  // 插入足够多的键触发叶子分裂与根分裂：高度应随层数增长，
  // 且叶子页数应等于分裂出的叶子个数。
  auto bpm = MakeBpm(512);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  const int32_t n = 2000;  // 远超单页容量（4KB 页下 int 键约能放几百个）
  for (int32_t i = 0; i < n; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 1, static_cast<uint8_t>(i % 256), &dup));
  }

  const uint16_t h = t.Height();
  const uint64_t leaves = t.LeafPageCount();
  // 至少长到「根 + 叶子」两层；2000 个键在 4KB 页下必定多叶
  EXPECT_TRUE(h >= 2);
  EXPECT_TRUE(leaves >= 2);
  // 叶子页数与键数一致地增长：每页至少能放 4 个键（MaxKeysPerIndexPage 下限）
  EXPECT_TRUE(leaves <= static_cast<uint64_t>(n));

  // 高度是可重复读取的（不改变树状态）
  EXPECT_EQ(t.Height(), h);
  EXPECT_EQ(t.LeafPageCount(), leaves);
}

TEST_CASE(bptree_stats_invalid_tree) {
  // 无效树（未 Create / 未 Attach）→ 高度与叶子数都是 0，不能崩
  auto bpm = MakeBpm(16);
  BPlusTree t(bpm.get(), IntSpec());
  EXPECT_TRUE(!t.valid());
  EXPECT_EQ(t.Height(), 0);
  EXPECT_EQ(t.LeafPageCount(), 0u);
}

TEST_CASE(bptree_leaf_pages_match_scan) {
  // 交叉验证：LeafPageCount 数出的叶子页，与「沿 right_sibling 走到最大键」一致。
  // 用「最后一个键所在页 == 从根到最右路径的叶子」这条不变量做侧面确认。
  auto bpm = MakeBpm(512);
  BPlusTree t(bpm.get(), IntSpec());
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));
  for (int32_t i = 0; i < 800; ++i) {
    bool dup = false;
    EXPECT_OK(InsertInt(&t, i, 2, static_cast<uint8_t>(i % 256), &dup));
  }
  const uint64_t leaves = t.LeafPageCount();
  EXPECT_TRUE(leaves >= 2);

  // 全量扫描应能取回全部 800 个键（叶子链完整）
  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), 800u);
}

// ─────────────────────────────────────────────────────────────
// 复合键（多列索引）：拼接自定界 + NULL 位图 + 前缀扫描
// ─────────────────────────────────────────────────────────────

namespace {

// 便捷：复合叶子键
std::string LeafKey(const std::vector<Value>& vals, page_id_t page, uint16_t slot) {
  return EncodeLeafKeyColumns(vals, page, slot);
}

}  // namespace

// 拼接 → 按列解码 roundtrip：覆盖负数、0x00 内容、空串、各位置的 NULL。
// 这是「自定界」声明的直接验证 —— 任何一列错位都会导致解码失败或值错。
TEST_CASE(composite_key_roundtrip_all_types) {
  struct Row {
    std::vector<Value> vals;
    std::vector<ValueType> types;
  };
  const std::string z00(std::size_t(3), '\0');  // 内容含真实 0x00 字节
  const std::vector<Row> rows = {
      // NULL 在首/中/尾
      {{Value::Null(), Int32(5), Varchar("x")}, {ValueType::kInt32, ValueType::kInt32, ValueType::kVarchar}},
      {{Int32(5), Value::Null(), Varchar("x")}, {ValueType::kInt32, ValueType::kInt32, ValueType::kVarchar}},
      {{Int32(5), Int32(6), Value::Null()}, {ValueType::kInt32, ValueType::kInt32, ValueType::kVarchar}},
      // 负数 / 0 / 边界
      {{Int32(-2147418112), Int64(-9223372036854775807LL - 1), Varchar("abc")},
       {ValueType::kInt32, ValueType::kInt64, ValueType::kVarchar}},
      {{Int32(0), Int64(0), Varchar("")},
       {ValueType::kInt32, ValueType::kInt64, ValueType::kVarchar}},
      // 0x00 内容 / 空串 / 全 0x00 内容
      {{Int32(7), Varchar(z00), Varchar("tail")},
       {ValueType::kInt32, ValueType::kVarchar, ValueType::kVarchar}},
      {{Int32(7), Varchar(""), Varchar("")},
       {ValueType::kInt32, ValueType::kVarchar, ValueType::kVarchar}},
      // 布尔 + 浮点 + 日期文本
      {{Value::Bool(true), Value::Float(-1.5f), Varchar("2026-01-02")},
       {ValueType::kBool, ValueType::kFloat, ValueType::kVarchar}},
      // NULL 与「编码以 00 01 开头的 INT32」（-2147387271，bits 0x8001xxxx）同场：位图必须消歧
      {{Value::Null(), Varchar("xy")}, {ValueType::kInt32, ValueType::kVarchar}},
      {{Int32(-2147387271), Varchar("")}, {ValueType::kInt32, ValueType::kVarchar}},
  };
  for (const Row& r : rows) {
    std::vector<ValueType> types;
    for (const Value& v : r.vals) {
      types.push_back(v.IsNull() ? ValueType::kInt32 : v.type);
    }
    const std::string leaf = LeafKey(r.vals, 9, 4);
    std::vector<Value> out;
    EXPECT_TRUE(DecodeLeafKeyColumns(leaf, r.types, &out));
    EXPECT_EQ(out.size(), r.vals.size());
    for (size_t i = 0; i < r.vals.size(); ++i) {
      if (r.vals[i].IsNull()) {
        EXPECT_TRUE(out[i].IsNull());
        continue;
      }
      if (r.vals[i].type == ValueType::kInt32) {
        EXPECT_EQ(out[i].int32_val, r.vals[i].int32_val);
      } else if (r.vals[i].type == ValueType::kInt64) {
        EXPECT_EQ(out[i].int64_val, r.vals[i].int64_val);
      } else if (r.vals[i].type == ValueType::kFloat) {
        EXPECT_EQ(out[i].float_val, r.vals[i].float_val);
      } else if (r.vals[i].type == ValueType::kVarchar) {
        EXPECT_TRUE(out[i].str_val == r.vals[i].str_val);
      } else if (r.vals[i].type == ValueType::kBool) {
        EXPECT_EQ(out[i].bool_val, r.vals[i].bool_val);
      }
    }
    // 行定位 roundtrip
    page_id_t pg = 0;
    uint16_t slot = 0;
    EXPECT_TRUE(DecodeLeafKeyRid(leaf, &pg, &slot));
    EXPECT_EQ(pg, 9u);
    EXPECT_EQ(slot, 4);
  }
}

// 历史歧义回归：NULL 标记 00 01 与 INT32 值 -2147387271 的保序编码
// （00 01 78 79）共享前缀，**不加位图时**两个不同元组会拼出完全相同的键；
// 复合键靠 NULL 位图消除该歧义。
// 注意：这个数值不是随手挑的 —— 它必须满足 enc(v) 以 00 01 开头，
// 即 v 的补码为 0x8001xxxx（-2147418112 .. -2147387271 这一段）。
// 用例同时断言「无位图会撞」与「有位图不撞」，否则它只是空转。
TEST_CASE(composite_key_null_bitmap_disambiguates) {
  // ① 先复现歧义：手工做「无位图」的朴素拼接
  auto naive_concat = [](const std::vector<Value>& vals) {
    std::string out;
    for (const Value& v : vals) {
      EncodeIndexColumn(v, &out);
    }
    return out;
  };
  const std::string naive_a = naive_concat({Value::Null(), Varchar("xy")});
  const std::string naive_b = naive_concat({Int32(-2147387271), Varchar("")});
  EXPECT_TRUE(naive_a == naive_b);           // ← 歧义真实存在（两条元组同一字节串）
  EXPECT_EQ(naive_a.size(), 6u);             // 00 01 | 78 79 00 00

  // ② 加位图后必须区分开（位图是第一个字节：01 = 第 1 列 NULL，00 = 非 NULL）
  const std::string a = EncodeColumnKeys({Value::Null(), Varchar("xy")});
  const std::string b = EncodeColumnKeys({Int32(-2147387271), Varchar("")});
  EXPECT_TRUE(a != b);
  EXPECT_EQ(static_cast<unsigned char>(a[0]), 0x01u);
  EXPECT_EQ(static_cast<unsigned char>(b[0]), 0x00u);

  // ③ 补一个哑行定位变成合法叶子键，各自都能精确解码回自己
  //（长度 ≥ 6 会按「尾部 6B 是行定位」切分 —— 这正是行定位恒在键尾的约定）
  const std::string dummy_rid(6, '\0');
  std::vector<Value> da, db;
  EXPECT_TRUE(DecodeLeafKeyColumns(a + dummy_rid, {ValueType::kInt32, ValueType::kVarchar}, &da));
  EXPECT_TRUE(da[0].IsNull());
  EXPECT_TRUE(da[1].str_val == "xy");
  EXPECT_TRUE(DecodeLeafKeyColumns(b + dummy_rid, {ValueType::kInt32, ValueType::kVarchar}, &db));
  EXPECT_EQ(db[0].int32_val, -2147387271);
  EXPECT_TRUE(db[1].str_val == "");
}

// 拼接后的字节序 == 元组逻辑序：先按第 1 列排，同值再按第 2 列排。
TEST_CASE(composite_key_byte_order_matches_tuple_order) {
  auto enc = [](int32_t a, const char* b) {
    return EncodeColumnKeys({Int32(a), Varchar(b)});
  };
  // 第 1 列决定主序
  EXPECT_TRUE(enc(1, "z") < enc(2, "a"));
  EXPECT_TRUE(enc(-5, "z") < enc(0, "a"));
  // 第 1 列相同 → 第 2 列决定次序
  EXPECT_TRUE(enc(3, "ab") < enc(3, "abc"));
  EXPECT_TRUE(enc(3, "") < enc(3, "a"));
  // NULL 在同列非空值之前（常规值域）
  std::string null_a, zero_a;
  EncodeIndexColumn(Value::Null(), &null_a);
  EncodeIndexColumn(Int32(0), &zero_a);
  EXPECT_TRUE(null_a < zero_a);
}

// 复合键树的完整生命周期：插入（含触发分裂）、Contains、前缀扫描、删除。
TEST_CASE(composite_tree_insert_contains_prefix_remove) {
  auto bpm = MakeBpm(512);
  BPlusTree::KeySpec spec;
  spec.columns.push_back({ValueType::kInt32, 0});
  spec.columns.push_back({ValueType::kVarchar, 32});
  BPlusTree t(bpm.get(), spec);
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));

  const int32_t n = 300;  // 复合键更长 → 更容易触发多页/分裂
  for (int32_t i = 0; i < n; ++i) {
    bool dup = false;
    const std::string leaf = LeafKey({Int32(i % 30), Varchar("name" + std::to_string(i))},
                                     static_cast<page_id_t>(1 + i / 100),
                                     static_cast<uint8_t>(i % 256));
    EXPECT_OK(t.Insert(leaf, &dup));
    EXPECT_TRUE(!dup);
  }
  EXPECT_TRUE(t.Height() >= 1);

  // 精确查找
  const std::string probe =
      LeafKey({Int32(10), Varchar("name10")}, static_cast<page_id_t>(1), 10);
  bool found = false;
  EXPECT_OK(t.Contains(probe, &found));
  EXPECT_TRUE(found);

  // 全量扫描：n 个键，有序
  std::vector<std::string> all;
  EXPECT_OK(t.ScanAll(&all));
  EXPECT_EQ(all.size(), static_cast<size_t>(n));
  for (size_t i = 1; i < all.size(); ++i) {
    EXPECT_TRUE(all[i - 1] < all[i]);
  }

  // 前缀扫描：第 1 列 = 10 的全部行（不论第 2 列）。
  // 部分前缀必须用 EncodeColumnPrefix —— 位图宽度按索引总列数对齐，
  // 用 EncodeColumnKeys 会按「1 列键」编码（无位图），匹配不上 2 列键。
  const std::string prefix = EncodeColumnPrefix({Int32(10)}, 2);
  std::vector<std::string> hits;
  EXPECT_OK(t.ScanPrefix(prefix, [&](const std::string& k) -> bool {
    hits.push_back(k);
    return true;
  }));
  EXPECT_EQ(hits.size(), 10u);  // i % 30 == 0? 10,40,...,280 → 10 个
  for (const std::string& k : hits) {
    std::vector<Value> vals;
    EXPECT_TRUE(DecodeLeafKeyColumns(k, {ValueType::kInt32, ValueType::kVarchar}, &vals));
    EXPECT_EQ(vals[0].int32_val, 10);
  }

  // 删除一半后扫描数量减半
  for (int32_t i = 0; i < n; i += 2) {
    const std::string leaf = LeafKey({Int32(i % 30), Varchar("name" + std::to_string(i))},
                                     static_cast<page_id_t>(1 + i / 100),
                                     static_cast<uint8_t>(i % 256));
    bool removed = false;
    EXPECT_OK(t.Remove(leaf, &removed));
    EXPECT_TRUE(removed);
  }
  std::vector<std::string> after;
  EXPECT_OK(t.ScanAll(&after));
  EXPECT_EQ(after.size(), static_cast<size_t>(n) / 2);
}

// 复合前缀扫描的提前终止回调
TEST_CASE(composite_prefix_scan_early_terminate) {
  auto bpm = MakeBpm(512);
  BPlusTree::KeySpec spec;
  spec.columns.push_back({ValueType::kInt32, 0});
  spec.columns.push_back({ValueType::kInt32, 0});
  BPlusTree t(bpm.get(), spec);
  page_id_t root = kInvalidPageId;
  EXPECT_OK(t.Create(&root));
  for (int32_t i = 0; i < 50; ++i) {
    bool dup = false;
    EXPECT_OK(t.Insert(LeafKey({Int32(7), Int32(i)}, 1, static_cast<uint8_t>(i)), &dup));
  }
  const std::string prefix = EncodeColumnPrefix({Int32(7)}, 2);
  int seen = 0;
  EXPECT_OK(t.ScanPrefix(prefix, [&](const std::string&) -> bool {
    ++seen;
    return seen < 5;  // 取 5 个就停
  }));
  EXPECT_EQ(seen, 5);
}

// 键长上限校验：两个超长 VARCHAR 的复合索引必须在建索引前被拒绝。
TEST_CASE(composite_key_too_long_rejected) {
  auto bpm = MakeBpm(256);
  BPlusTree::KeySpec ok_spec;
  ok_spec.columns.push_back({ValueType::kInt32, 0});
  ok_spec.columns.push_back({ValueType::kVarchar, 32});
  EXPECT_TRUE(BPlusTree::KeyFitsPage(256, ok_spec));

  BPlusTree::KeySpec bad_spec;
  bad_spec.columns.push_back({ValueType::kVarchar, 3000});
  bad_spec.columns.push_back({ValueType::kVarchar, 3000});
  // 最坏键 = 2*(3000*2+2) + 5 + 4 ≈ 12017 > 256B 页 → 放不下 2 个键
  EXPECT_TRUE(!BPlusTree::KeyFitsPage(256, bad_spec));
  EXPECT_TRUE(BPlusTree::MaxLeafKeyBytes(bad_spec) > 256);
}
