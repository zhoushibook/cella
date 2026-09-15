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
#include "console_utf8.h"   // 扁平名：demo/examples 只挂了扁平公共目录

using namespace cella::storage;

// 临时诊断程序：逐层打印 B+ 树结构，用来定位降序插入丢键的问题。
// 用法: bptree_diag.exe [N]

namespace {

std::unique_ptr<BufferPoolManager> MakeBpm(size_t pool) {
  auto disk = std::make_unique<MemDiskManager>(4096);
  (void)disk->Open("");
  auto replacer = ReplacerFactory::Create("LRU", pool);
  return std::make_unique<BufferPoolManager>(pool, std::move(disk), std::move(replacer));
}

Value Int32(int32_t v) { return Value::Int(v); }

std::string KeyOf(int32_t v, uint8_t slot) { return EncodeLeafKey(Int32(v), 1, slot); }

// 收集从某页可达的所有键（递归），返回 (leaf_keys, total_nodes, leaf_count)
void DumpTree(BufferPoolManager* bpm, page_id_t pid, int depth,
              std::vector<std::string>* leaf_keys, int* nodes, int* leaves) {
  PageGuard g(bpm, bpm->get_page(pid));
  if (!g.valid()) {
    std::printf("  %*s[无效页 %u]\n", depth * 2, "", pid);
    return;
  }
  ++(*nodes);
  IndexNode node(g.get());
  std::printf("  %*s页%u type=%s keys=%u max=%u sib=%u\n", depth * 2, "", pid,
              node.type() == IndexNodeType::kLeaf ? "LEAF" : "INT", node.key_count(),
              node.max_keys(), node.right_sibling());
  for (size_t i = 0; i < node.key_count(); ++i) {
    std::string k;
    node.GetKey(i, &k);
    std::string col = StripLeafRowId(k);
    int32_t v = 0;
    Value out;
    if (DecodeLeafKeyColumn(k, ValueType::kInt32, &out)) {
      v = out.int32_val;
    }
    std::printf("  %*s  [%zu] col=%d keylen=%zu\n", depth * 2, "", i, v, col.size());
  }
  if (node.type() == IndexNodeType::kLeaf) {
    ++(*leaves);
    for (size_t i = 0; i < node.key_count(); ++i) {
      std::string k;
      if (node.GetKey(i, &k)) {
        leaf_keys->push_back(k);
      }
    }
    return;
  }
  for (size_t i = 0; i <= node.key_count(); ++i) {
    DumpTree(bpm, node.Child(i), depth + 1, leaf_keys, nodes, leaves);
  }
}

}  // namespace

int main(int argc, char** argv) {
  cella::storage::EnableUtf8Console();   // 控制台按 UTF-8 显示中文
  const int32_t N = (argc > 1) ? std::atoi(argv[1]) : 600;
  auto bpm = MakeBpm(512);
  BPlusTree::KeySpec spec = BPlusTree::KeySpec::Single(ValueType::kInt32, 0);
  BPlusTree t(bpm.get(), spec);
  page_id_t root = kInvalidPageId;
  (void)t.Create(&root);
  std::printf("root=%u\n", root);

  int fail_at = -1;
  for (int32_t i = N; i > 0; --i) {
    bool dup = false;
    const Status s = t.Insert(KeyOf(i, static_cast<uint8_t>(i % 200)), &dup);
    if (!s.ok()) {
      std::printf("! 插入 %d 失败: %s\n", i, s.ToString().c_str());
      break;
    }
  }
  (void)fail_at;

  std::printf("=== 树结构 ===\n");
  // 注意：必须用 t.root_page() 而不是 Create() 时拿到的局部 root ——
  // 插入过程中根可能分裂（树长高一层），此时 root_ 已指向新的内部根，
  // 而局部变量还停在最初的叶子页上（用旧根遍历会「只看到第一片叶子」，
  // 表现为「收集到 184 个键（期望 600）」这种假故障）。
  {
    const page_id_t final_root = t.root_page();
    std::printf("初始 root=%u → 最终 root=%u%s\n", root, final_root,
                final_root == root ? "（未发生根分裂）" : "（根已分裂，树长高）");
    std::printf("树高 = %u 层，叶子页 = %llu 页\n", t.Height(),
                static_cast<unsigned long long>(t.LeafPageCount()));
  }
  std::vector<std::string> leaves;
  int nodes = 0, leaves_n = 0;
  DumpTree(bpm.get(), t.root_page(), 0, &leaves, &nodes, &leaves_n);

  // 另用叶子链把整棵树串一遍（DumpTree 只走父子路径，看不到兄弟链）
  {
    PageGuard g(bpm.get(), bpm->get_page(t.root_page()));
    IndexNode n(g.get());
    std::printf("root type=%s\n", n.type() == IndexNodeType::kLeaf ? "LEAF" : "INT");
  }
  std::printf("=== 共 %d 节点（其中 %d 叶子），收集到 %zu 个键（期望 %d）===\n",
              nodes, leaves_n, leaves.size(), N);

  std::set<int32_t> got;
  for (const auto& k : leaves) {
    Value v;
    if (DecodeLeafKeyColumn(k, ValueType::kInt32, &v)) {
      got.insert(v.int32_val);
    }
  }
  std::printf("去重后 %zu 个不同列值\n", got.size());
  int missing = 0;
  for (int32_t i = 1; i <= N; ++i) {
    if (got.find(i) == got.end()) {
      if (missing < 20) {
        std::printf("  缺失列值: %d\n", i);
      }
      ++missing;
    }
  }
  std::printf("缺失合计 %d\n", missing);

  // 用公开扫描 API 再数一遍
  std::vector<std::string> scanned;
  (void)t.ScanAll(&scanned);
  std::printf("ScanAll 得到 %zu 个键\n", scanned.size());
  return 0;
}
