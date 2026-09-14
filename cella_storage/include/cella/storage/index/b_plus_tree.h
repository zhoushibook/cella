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
// BPlusTree —— B+ 树索引（键 = 编码后的列值元组 + 行定位 Rid）。
//
// 结构（标准 B+ 树）：
//   * 所有数据都在叶子层，叶子用 right_sibling 串成有序链表 → 范围扫描容易；
//   * 内部节点只存「分隔键」：keys[i] 是第 i+1 棵子树的最小键，
//     因此查找时取「最后一个 <= key 的 keys[i]」走 children[i]；
//   * 插入自底向上分裂；根分裂时树长高一层。
//
// 键是**变长**的字节串（见 index_key.h）。比较统一走 CompareIndexKey：
//   - 叶子键 = 列值元组编码 + 行定位（5B）
//   - 内部键 = 列值元组编码（无行定位）→ 与叶子键比较时用「前缀比较」。
//     行定位恒在键尾 5 字节 → 去尾后仍是完整元组编码，前缀比较对复合键
//     继续成立（StripLeafRowId 不需要知道列数）。
//
// 支持单列与多列复合键（KeySpec::columns）：单列是列数为 1 的特例，
// 不写任何特殊分支；复合键的拼接/解码细节见 index_key.h（NULL 位图）。
//
// 并发：不做内部加锁。上层（cella_db）的存储互斥量已把存储访问串行化，
//       本类沿用 IStorage「单线程设计」的既有约定。
//
// 生命周期：由 IndexHandle 之外的调用方（DbEngine）持有；Open 时用
//          root_page 重建，Close 无需特殊处理（页由缓冲池统一刷盘）。
// ─────────────────────────────────────────────────────────────────────────

class BPlusTree {
 public:
  // 键的列规格（决定编码/解码方式与最大键长）
  struct Column {
    ValueType type = ValueType::kInt32;
    uint16_t max_len = 0;        // VARCHAR/CHAR 的最大字节长（定长类型忽略）
  };

  // 列序列表：单列索引 = 1 个元素；复合索引按声明序排列。
  struct KeySpec {
    std::vector<Column> columns;

    // 单列便捷构造（既有调用方/测试用）
    static KeySpec Single(ValueType type, uint16_t max_len) {
      KeySpec s;
      s.columns.push_back(Column{type, max_len});
      return s;
    }
    bool empty() const { return columns.empty(); }
    size_t column_count() const { return columns.size(); }
  };

  BPlusTree(BufferPoolManager* bpm, KeySpec spec);

  // ── 键长上限（建索引时校验用）─────────────────────────────
  // 最坏叶子键字节数 = NULL 位图（复合键）+ Σ(各列最大编码) + 行定位 5B。
  // VARCHAR 转义后最长 2n+2 —— 多个长 VARCHAR 组合可能撑爆一页，
  // 必须在建索引时拒绝，而不是等插入时才发现放不下。
  static size_t MaxLeafKeyBytes(const KeySpec& spec);
  // 页里必须至少放得下 min_keys 个最坏键（一页至少 2~3 个键才能分裂）。
  // 建索引前调用：超限返回 false，调用方直接拒绝建索引。
  static bool KeyFitsPage(uint32_t page_size, const KeySpec& spec, size_t min_keys = 2);

  // ── 生命周期 ──────────────────────────────────────────────
  // 建一棵空树（分配根叶子页）。成功后 root_page 有效。
  Status Create(page_id_t* out_root);
  // 绑定已有根页（从目录元数据恢复）。
  void Attach(page_id_t root) { root_ = root; }
  page_id_t root_page() const { return root_; }
  bool valid() const { return root_ != kInvalidPageId; }

  // ── 插入 ──────────────────────────────────────────────────
  // key 必须由 EncodeLeafKeyColumns 生成（含行定位）。
  // duplicate = true 表示遇到完全相同的键（同列值 + 同 Rid）→ 重复插入。
  // unique 检查由上层做（需要区分「同列值不同行」与「完全重复」），
  // 本层提供前缀扫描原语供上层查询。
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

  // ── 前缀扫描（复合索引「最左前缀」的原语）─────────────────
  // prefix 是 EncodeColumnKeys 的输出（NULL 位图 + 前几列的编码）。
  // 输出所有「列值部分以 prefix 为字节前缀」的叶子键 —— 即元组前缀匹配。
  // 为什么不用 ScanRange 硬凑 hi 边界：VARCHAR 内容可以含真实 0xFF 字节，
  // 「enc(a=v) + 0xFF…」这样的排他上界凑不出来；字节前缀比较不需要解码，
  // 天然正确。回调返回 false 可提前终止。
  Status ScanPrefix(const std::string& prefix, const ScanCallback& cb);

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
