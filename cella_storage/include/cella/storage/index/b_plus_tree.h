#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cella/storage/common/status.h"
#include "cella/storage/common/types.h"
#include "cella/storage/common/value.h"
#include "cella/storage/index/index_node.h"   // IndexNodeType / kIndexRowIdSize

namespace cella::storage {

class BufferPoolManager;

// ─────────────────────────────────────────────────────────────────────────
// BPlusTree —— 单列 B+ 树索引（键 = 编码后的列值 + 行定位 Rid）。
//
// 结构（标准 B+ 树）：
//   * 所有数据都在叶子层，叶子用 right_sibling 串成有序链表 → 范围扫描容易；
//   * 内部节点只存「分隔键」：keys[i] 是第 i+1 棵子树的最小键，
//     因此查找时取「最后一个 <= key 的 keys[i]」走 children[i]；
//   * 插入自底向上分裂；根分裂时树长高一层。
//
// 键是**变长**的字节串（见 index_key.h）。比较统一走 CompareIndexKey：
//   - 叶子键 = 列值编码 + 行定位（5B）
//   - 内部键 = 列值编码（无行定位）→ 与叶子键比较时用「前缀比较」
//     这是 B+ 树变长键实现里最容易出错的地方，比较函数里已显式处理。
//
// 并发：不做内部加锁。上层（cella_db）的存储互斥量已把存储访问串行化，
//       本类沿用 IStorage「单线程设计」的既有约定。
//
// 生命周期：由 IndexHandle 之外的调用方（DbEngine）持有；Open 时用
//          root_page 重建，Close 无需特殊处理（页由缓冲池统一刷盘）。
// ─────────────────────────────────────────────────────────────────────────

class BPlusTree {
 public:
  // 键列的类型（决定解码方式与最大键长）
  struct KeySpec {
    ValueType type = ValueType::kInt32;
    uint16_t max_len = 0;        // VARCHAR/CHAR 的最大字节长（定长类型忽略）
  };

  BPlusTree(BufferPoolManager* bpm, KeySpec spec);

  // ── 生命周期 ──────────────────────────────────────────────
  // 建一棵空树（分配根叶子页）。成功后 root_page 有效。
  Status Create(page_id_t* out_root);
  // 绑定已有根页（从目录元数据恢复）。
  void Attach(page_id_t root) { root_ = root; }
  page_id_t root_page() const { return root_; }
  bool valid() const { return root_ != kInvalidPageId; }

  // ── 插入 ──────────────────────────────────────────────────
  // key 必须由 EncodeLeafKey 生成（含行定位）。
  // duplicate = true 表示遇到完全相同的键（同列值 + 同 Rid）→ 重复插入。
  // unique 检查由上层做（需要区分「同列值不同行」与「完全重复」），
  // 本层只提供 ContainsKeyValue 供上层查询。
  Status Insert(const std::string& leaf_key, bool* duplicate);

  // ── 删除 ──────────────────────────────────────────────────
  Status Remove(const std::string& leaf_key, bool* removed);

  // ── 精确查找 ──────────────────────────────────────────────
  // 键完全匹配（列值 + 行定位）返回 true。
  Status Contains(const std::string& leaf_key, bool* found);

  // ── 范围扫描 ──────────────────────────────────────────────
  // 按 encode 后的边界键扫描叶子：
  //   lo == nullptr → 从最小开始；hi == nullptr → 到最大结束
  //   include_hi = false 时排除「列值部分等于 hi 前缀」的所有行
  // 回调收到每个叶子键（含行定位）。回调返回 false 可提前终止。
  using ScanCallback = std::function<bool(const std::string& leaf_key)>;
  Status ScanRange(const std::string* lo_column_key, const std::string* hi_column_key,
                   bool include_hi, const ScanCallback& cb);

  using ScanLeafCallback = std::function<bool(const std::string& leaf_key)>;
  Status Scan(ScanLeafCallback cb);

  // 全量扫描（诊断用，含行定位）
  Status ScanAll(std::vector<std::string>* out);

  // 键列类型（上层解码用）
  const KeySpec& key_spec() const { return spec_; }

  // ── 统计信息（P1.6 代价模型用）───────────────────────────
  // 树的层数：叶子层 = 1。空树/无效树返回 0。
  // 实现是「一直往最左子指针走到底」，O(树高)，不遍历整棵树。
  // 为什么需要它：代价模型要按「一次索引定位花几页随机 I/O」计费，
  // 而早期版本用固定常数 2.0，在小表上把一次等值查找估得比全表扫描还贵
  // （200 行表：索引 16.05 vs 全表 15.0），导致主键查找选不中主键索引。
  uint16_t Height() const;

  // 叶子页数量：沿 right_sibling 链表数一遍，O(叶子数)。
  // 用于估算索引自身的大小（全索引扫描代价）与「回表页数」的上界。
  // 失败（页损坏）时返回 0。
  uint64_t LeafPageCount() const;

 private:
  // ── 内部：节点读写（都在一次 get_page/PageGuard 内完成）──
  struct SearchPath {
    std::vector<page_id_t> pages;   // 从根到叶的路径
    std::vector<size_t> positions;  // 每一步选择的子下标
  };

  // 找到 key 应落入的叶子页
  Status FindLeaf(page_id_t* leaf, const std::string& leaf_key);
  // 在节点内定位：返回第一个 >= key 的键下标
  size_t LowerBound(uint16_t key_count, const std::string& key,
                    const std::function<bool(size_t, std::string*)>& get_key,
                    bool child_keys_are_prefix);

  // 递归插入（返回是否发生分裂）
  struct SplitResult {
    bool split = false;
    std::string sep_key;      // 上提的分隔键（列值形态）
    page_id_t right_page = kInvalidPageId;
  };
  Status InsertInto(page_id_t node, const std::string& leaf_key, bool* duplicate,
                    SplitResult* out);

  Status NewIndexPage(IndexNodeType type, page_id_t* out);
  uint16_t MaxKeys(bool internal) const;

  BufferPoolManager* bpm_;
  KeySpec spec_;
  page_id_t root_ = kInvalidPageId;
};

}  // namespace cella::storage
