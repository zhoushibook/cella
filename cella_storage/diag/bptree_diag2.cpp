#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/buffer/replacer_factory.h"
#include "cella/storage/disk/mem_disk_manager.h"
#include "cella/storage/index/b_plus_tree.h"
#include "cella/storage/index/index_key.h"
#include "cella/storage/index/index_node.h"
#include "cella/storage/page/page.h"

using namespace cella::storage;

// 诊断 2：跟踪每次分裂 + 根变化，定位「树不升高」的原因。
// 用法: bptree_diag2.exe [N] [asc|desc]

namespace {

std::unique_ptr<BufferPoolManager> MakeBpm(size_t pool) {
  auto disk = std::make_unique<MemDiskManager>(4096);
  (void)disk->Open("");
  auto replacer = ReplacerFactory::Create("LRU", pool);
  return std::make_unique<BufferPoolManager>(pool, std::move(disk), std::move(replacer));
}

Value Int32(int32_t v) { return Value::Int(v); }

}  // namespace

int main(int argc, char** argv) {
  const int32_t N = (argc > 1) ? std::atoi(argv[1]) : 600;
  const bool desc = (argc > 2 && std::string(argv[2]) == "desc");

  auto bpm = MakeBpm(2048);
  BPlusTree::KeySpec spec;
  spec.type = ValueType::kInt32;
  BPlusTree t(bpm.get(), spec);
  page_id_t root = kInvalidPageId;
  (void)t.Create(&root);
  std::printf("初始 root=%u\n", root);

  page_id_t prev_root = root;
  for (int32_t i = 0; i < N; ++i) {
    const int32_t v = desc ? (N - i) : (i + 1);
    bool dup = false;
    const Status s = t.Insert(EncodeLeafKey(Int32(v), 1, static_cast<uint8_t>(v % 200)), &dup);
    if (!s.ok()) {
      std::printf("! 插入 v=%d 失败: %s\n", v, s.ToString().c_str());
      break;
    }
    if (t.root_page() != prev_root) {
      std::printf("  [根变化] i=%d v=%d: %u -> %u\n", i, v, prev_root, t.root_page());
      prev_root = t.root_page();
    }
  }
  std::printf("最终 root=%u\n", t.root_page());

  // 报告根类型与键数
  {
    PageGuard g(bpm.get(), bpm->get_page(t.root_page()));
    IndexNode n(g.get());
    std::printf("根节点: type=%s keys=%u max=%u\n",
                n.type() == IndexNodeType::kLeaf ? "LEAF" : "INT", n.key_count(), n.max_keys());
  }

  // 沿根递归收集叶子键
  std::vector<std::string> leaves;
  std::function<void(page_id_t)> rec = [&](page_id_t pid) {
    PageGuard g(bpm.get(), bpm->get_page(pid));
    IndexNode n(g.get());
    if (n.type() == IndexNodeType::kLeaf) {
      for (size_t i = 0; i < n.key_count(); ++i) {
        std::string k;
        if (n.GetKey(i, &k)) {
          leaves.push_back(k);
        }
      }
      return;
    }
    for (size_t i = 0; i <= n.key_count(); ++i) {
      rec(n.Child(i));
    }
  };
  rec(t.root_page());
  std::printf("沿根可达叶子键数 = %zu (期望 %d)\n", leaves.size(), N);

  std::vector<std::string> scanned;
  (void)t.ScanAll(&scanned);
  std::printf("ScanAll = %zu\n", scanned.size());

  int missing = 0;
  std::set<int32_t> got;
  for (const auto& k : leaves) {
    Value v;
    if (DecodeLeafKeyColumn(k, ValueType::kInt32, &v)) {
      got.insert(v.int32_val);
    }
  }
  for (int32_t i = 1; i <= N; ++i) {
    if (got.find(i) == got.end()) {
      ++missing;
    }
  }
  std::printf("缺失 = %d\n", missing);

  // 查询验证：随机抽查
  int qbad = 0;
  for (int32_t i = 1; i <= N; ++i) {
    bool found = false;
    (void)t.Contains(EncodeLeafKey(Int32(i), 1, static_cast<uint8_t>(i % 200)), &found);
    if (!found) {
      ++qbad;
    }
  }
  std::printf("查不到的键 = %d\n", qbad);
  return 0;
}
