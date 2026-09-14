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
  BPlusTree::KeySpec s;
  s.type = ValueType::kInt32;
  s.max_len = 0;
  return s;
}

Value Int32(int32_t v) { return Value::Int(v); }

Value Varchar(const std::string& s) { return Value::Varchar(s); }

// 便捷：插入一个 int 值
Status InsertInt(BPlusTree* t, int32_t v, page_id_t page, uint8_t slot, bool* dup) {
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
  uint8_t slot = 0;
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
  BPlusTree::KeySpec spec;
  spec.type = ValueType::kVarchar;
  spec.max_len = 32;

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
