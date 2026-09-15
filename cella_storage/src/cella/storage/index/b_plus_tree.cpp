#include "cella/storage/index/b_plus_tree.h"

#include <utility>

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/index/index_key.h"
#include "cella/storage/index/index_node.h"

namespace cella::storage {

namespace {

// 在「节点内的键数组」里找第一个 >= target 的位置（标准 lower_bound）。
// node_keys_are_prefix：节点里的键是「无行定位的列值键」，
// 而 target 是叶子键（含行定位）→ 需要按「节点键是 target 的前缀」来比较。
// 返回 [0, key_count]。
size_t LowerBoundInNode(const IndexNode& node, const std::string& target,
                       bool node_keys_are_prefix) {
  size_t lo = 0;
  size_t hi = node.key_count();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    std::string k;
    if (!node.GetKey(mid, &k)) {
      break;
    }
    // 统一问：「node 键 < target 吗？」
    //   节点键是前缀形态 → CompareIndexKey(target, k, /*b_is_prefix=*/true) > 0 表示 target > k
    //   否则            → CompareIndexKey(k, target, false) < 0 表示 k < target
    const bool node_less = node_keys_are_prefix ? (CompareIndexKey(target, k, true) > 0)
                                                : (CompareIndexKey(k, target, false) < 0);
    if (node_less) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// 内部节点下落：返回应进入的子下标。
//
// 不变式：children[i+1] 的最小键 == keys[i]。因此要走 children[i]，
// 需要 i = 「keys 中小于等于 target 的个数」——注意是 <= 而不是 <：
// 若 target 正好等于 keys[i]，目标仍可能在该子树的第一个位置，
// 必须往有 side 走（B+ 树里相同的键总是落在左子树）。
size_t ChildIndexFor(const IndexNode& node, const std::string& target) {
  size_t lo = 0;
  size_t hi = node.key_count();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    std::string k;
    if (!node.GetKey(mid, &k)) {
      break;
    }
    // k <= target ?（内部键是列值形态，target 含行定位 → 前缀比较）
    const bool k_le = CompareIndexKey(target, k, true) >= 0;
    if (k_le) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;   // ∈ [0, key_count]
}

}  // namespace

BPlusTree::BPlusTree(BufferPoolManager* bpm, KeySpec spec) : bpm_(bpm), spec_(spec) {}

// ── 键长上限（建索引时校验用）───────────────────────────────
// 最坏叶子键 = NULL 位图（复合键）+ Σ(各列最大编码) + 行定位 6B。
size_t BPlusTree::MaxLeafKeyBytes(const KeySpec& spec) {
  size_t total = NullBitmapBytes(spec.columns.size());
  for (const Column& c : spec.columns) {
    total += MaxEncodedColumnLen(c.type, c.max_len);
  }
  return total + kIndexLeafRidBytes;
}

bool BPlusTree::KeyFitsPage(uint32_t page_size, const KeySpec& spec, size_t min_keys) {
  if (spec.empty()) {
    return false;
  }
  // 每个最坏键占：键本体 + 槽项 4B（行定位已计入 MaxLeafKeyBytes）
  const size_t per_key = MaxLeafKeyBytes(spec) + 4;
  // 页内可用 = 页大小 - 数据区起点 - 至少 1 个子指针位（内部节点骨架）
  const size_t avail = static_cast<size_t>(page_size) - kIndexDataBegin - 4;
  return avail / per_key >= min_keys;
}

uint16_t BPlusTree::MaxKeys(bool internal) const {
  // 键的最大编码长度：定长列可精确算，变长按 max_len 上限 + 结尾标记 + 转义余量；
  // 复合键还要加 NULL 位图。对单列与复合键用同一套求和逻辑（单列 = 1 项）。
  size_t max_key = NullBitmapBytes(spec_.columns.size());
  for (const Column& c : spec_.columns) {
    max_key += MaxEncodedColumnLen(c.type, c.max_len);
  }
  const uint32_t page_size = bpm_->page_size();
  return MaxKeysPerIndexPage(page_size, max_key, internal);
}

Status BPlusTree::NewIndexPage(IndexNodeType type, page_id_t* out) {
  page_id_t pid = kInvalidPageId;
  Page* p = bpm_->NewPage(&pid);
  if (p == nullptr) {
    return Status::Error(StatusCode::kNoFreePage, "无法分配索引页");
  }
  {
    PageGuard g(bpm_, p);
    g->SetHeaderPageId(pid);
    IndexNode node(g.get());
    node.Init(type, MaxKeys(type == IndexNodeType::kInternal));
    g.MarkDirty();
  }
  *out = pid;
  return Status::OK();
}

Status BPlusTree::Create(page_id_t* out_root) {
  page_id_t root = kInvalidPageId;
  const Status s = NewIndexPage(IndexNodeType::kLeaf, &root);
  if (!s.ok()) {
    return s;
  }
  root_ = root;
  *out_root = root;
  return Status::OK();
}

Status BPlusTree::FindLeaf(page_id_t* leaf, const std::string& leaf_key) {
  page_id_t cur = root_;
  for (;;) {
    PageGuard g(bpm_, bpm_->get_page(cur));
    if (!g.valid()) {
      return Status::Error(StatusCode::kPageNotFound, "索引页不存在: " + std::to_string(cur));
    }
    // 探类型：type() 只读节点头，与 max_keys 无关，可先用叶子的几何探一下
    IndexNode probe(g.get());
    if (probe.type() == IndexNodeType::kLeaf) {
      *leaf = cur;
      return Status::OK();
    }
    // 内部节点：几何按内部容量解释（slot_base 由 max_keys 决定）
    IndexNode inode(g.get());
    const size_t pos = ChildIndexFor(inode, leaf_key);
    const page_id_t next = inode.Child(pos);
    if (next == kInvalidPageId) {
      return Status::Error(StatusCode::kCorruptPage, "索引内部节点子指针为空");
    }
    cur = next;
  }
}

Status BPlusTree::Contains(const std::string& leaf_key, bool* found) {
  *found = false;
  if (!valid()) {
    return Status::OK();
  }
  page_id_t leaf = kInvalidPageId;
  const Status s = FindLeaf(&leaf, leaf_key);
  if (!s.ok()) {
    return s;
  }
  PageGuard g(bpm_, bpm_->get_page(leaf));
  if (!g.valid()) {
    return Status::Error(StatusCode::kPageNotFound, "索引叶子页不存在");
  }
  IndexNode node(g.get());
  // 叶子键含行定位 → 与叶子键做完整比较
  size_t pos = LowerBoundInNode(node, leaf_key, false);
  while (pos < node.key_count()) {
    std::string k;
    if (!node.GetKey(pos, &k)) {
      break;
    }
    if (k == leaf_key) {
      *found = true;
      return Status::OK();
    }
    if (CompareIndexKey(k, leaf_key, false) > 0) {
      break;
    }
    ++pos;
  }
  return Status::OK();
}

// ── 插入 ────────────────────────────────────────────────────

Status BPlusTree::Insert(const std::string& leaf_key, bool* duplicate) {
  *duplicate = false;
  if (!valid()) {
    return Status::Error(StatusCode::kInvalidArgument, "索引未初始化");
  }
  SplitResult sr;
  const Status s = InsertInto(root_, leaf_key, duplicate, &sr);
  if (!s.ok()) {
    return s;
  }
  if (sr.split) {
    // 根分裂：新建内部根，唯一键 = 上提分隔键
    page_id_t new_root = kInvalidPageId;
    const Status cs = NewIndexPage(IndexNodeType::kInternal, &new_root);
    if (!cs.ok()) {
      return cs;
    }
    PageGuard g(bpm_, bpm_->get_page(new_root));
    if (!g.valid()) {
      return Status::Error(StatusCode::kPageNotFound, "新根页读取失败");
    }
    IndexNode node(g.get());
    node.SetChild(0, root_);
    node.SetChild(1, sr.right_page);
    const Status is = node.InsertKeyAt(0, sr.sep_key);
    if (!is.ok()) {
      return is;
    }
    g.MarkDirty();
    root_ = new_root;
  }
  return Status::OK();
}

Status BPlusTree::InsertInto(page_id_t node_page, const std::string& leaf_key, bool* duplicate,
                             SplitResult* out) {
  PageGuard g(bpm_, bpm_->get_page(node_page));
  if (!g.valid()) {
    return Status::Error(StatusCode::kPageNotFound, "索引页不存在");
  }
  IndexNode node(g.get());
  const bool is_leaf = (node.type() == IndexNodeType::kLeaf);

  if (is_leaf) {
    // 叶子：找插入位置（叶子键完整比较）
    size_t pos = LowerBoundInNode(node, leaf_key, false);
    // 检查是否已存在完全相同的键
    if (pos < node.key_count()) {
      std::string k;
      if (node.GetKey(pos, &k) && k == leaf_key) {
        *duplicate = true;
        return Status::OK();   // 完全重复，由上层决定是否报错
      }
    }
    if (node.FreeSpace() >= leaf_key.size() + 4) {
      const Status s = node.InsertKeyAt(pos, leaf_key);
      if (!s.ok()) {
        return s;
      }
      g.MarkDirty();
      return Status::OK();
    }
    // 空间不足 → 分裂
    // 收集全部键 + 新键，排序后二分
    std::vector<std::string> keys;
    keys.reserve(node.key_count() + 1);
    for (size_t i = 0; i < node.key_count(); ++i) {
      std::string k;
      if (node.GetKey(i, &k)) {
        keys.push_back(std::move(k));
      }
    }
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(pos), leaf_key);
    const size_t mid = keys.size() / 2;

    page_id_t right = kInvalidPageId;
    const Status ns = NewIndexPage(IndexNodeType::kLeaf, &right);
    if (!ns.ok()) {
      return ns;
    }
    // 左页保留 [0, mid)，右页放 [mid, end)
    // 注意：Clear() 会把 right_sibling 重置为无效值，因此必须先存下旧的兄弟指针，
    // 否则重建后右页的链会断掉（叶子链表不连续 → 范围扫描丢行）。
    const page_id_t old_sibling = node.right_sibling();
    node.Clear();
    {
      PageGuard rg(bpm_, bpm_->get_page(right));
      if (!rg.valid()) {
        return Status::Error(StatusCode::kPageNotFound, "分裂右页读取失败");
      }
      IndexNode rn(rg.get());
      for (size_t i = 0; i < keys.size(); ++i) {
        if (i < mid) {
          const Status s = node.InsertKeyAt(i, keys[i]);
          if (!s.ok()) {
            return s;
          }
        } else {
          const Status s = rn.InsertKeyAt(i - mid, keys[i]);
          if (!s.ok()) {
            return s;
          }
        }
      }
      rn.SetRightSibling(old_sibling);
      rg.MarkDirty();
    }
    node.SetRightSibling(right);
    g.MarkDirty();

    out->split = true;
    out->right_page = right;
    // 上提的分隔键 = 右页第一个键的**列值形态**（去掉行定位）
    std::string first_right;
    {
      PageGuard rg(bpm_, bpm_->get_page(right));
      IndexNode rn(rg.get());
      (void)rn.GetKey(0, &first_right);
    }
    out->sep_key = StripLeafRowId(first_right);
    return Status::OK();
  }

  // 内部节点：递归到子节点
  IndexNode inode(g.get());
  const size_t pos = ChildIndexFor(inode, leaf_key);
  const page_id_t child = inode.Child(pos);
  SplitResult child_split;
  const Status cs = InsertInto(child, leaf_key, duplicate, &child_split);
  if (!cs.ok() || *duplicate || !child_split.split) {
    return cs;
  }
  // 子节点分裂 → 在本节点插入分隔键 + 新子指针
  // 新分隔键插到 keys[pos]，对应的新子指针插到 children[pos+1]。
  // （插入前 keys 有 n_old 个、children 有 n_old+1 个。）
  // 注意：子指针数组容量是 max_keys+1，插入后为 n_old+2，必须 <= max_keys+1，
  // 即 n_old + 1 <= max_keys。否则要走分裂分支，不能就地插入。
  const size_t n_old = inode.key_count();
  if (n_old + 1 <= static_cast<size_t>(inode.max_keys()) &&
      inode.FreeSpace() >= child_split.sep_key.size() + 4) {
    // 先把 children[pos+1 .. n_old] 整体后移一格，为新子指针腾位
    for (size_t i = n_old + 1; i > pos + 1; --i) {
      inode.SetChild(i, inode.Child(i - 1));
    }
    inode.SetChild(pos + 1, child_split.right_page);
    const Status s = inode.InsertKeyAt(pos, child_split.sep_key);
    if (!s.ok()) {
      return s;
    }
    g.MarkDirty();
    return Status::OK();
  }
  // 本节点也满了 → 分裂（把 pos 处插入后再二分）
  std::vector<std::string> keys;
  std::vector<page_id_t> children;
  for (size_t i = 0; i < inode.key_count(); ++i) {
    std::string k;
    if (inode.GetKey(i, &k)) {
      keys.push_back(std::move(k));
    }
    children.push_back(inode.Child(i));
  }
  children.push_back(inode.Child(inode.key_count()));
  keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(pos), child_split.sep_key);
  children.insert(children.begin() + static_cast<std::ptrdiff_t>(pos) + 1, child_split.right_page);

  const size_t mid = keys.size() / 2;
  const std::string up = keys[mid];

  page_id_t right = kInvalidPageId;
  const Status ns = NewIndexPage(IndexNodeType::kInternal, &right);
  if (!ns.ok()) {
    return ns;
  }
  node.Clear();
  {
    PageGuard rg(bpm_, bpm_->get_page(right));
    if (!rg.valid()) {
      return Status::Error(StatusCode::kPageNotFound, "内部节点分裂右页读取失败");
    }
    IndexNode rn(rg.get());
    // keys[mid] 上提到父节点，不再留在任一侧。
    // 左：keys[0, mid)，children[0, mid]
    for (size_t i = 0; i < mid; ++i) {
      const Status s = node.InsertKeyAt(i, keys[i]);
      if (!s.ok()) {
        return s;
      }
    }
    for (size_t i = 0; i <= mid; ++i) {
      node.SetChild(i, children[i]);
    }
    // 右：keys[mid+1, end)，children[mid+1, end]
    for (size_t i = mid + 1; i < keys.size(); ++i) {
      const Status s = rn.InsertKeyAt(i - mid - 1, keys[i]);
      if (!s.ok()) {
        return s;
      }
    }
    for (size_t i = mid + 1; i < children.size(); ++i) {
      rn.SetChild(i - mid - 1, children[i]);
    }
    rg.MarkDirty();
  }
  g.MarkDirty();

  out->split = true;
  out->sep_key = up;
  out->right_page = right;
  return Status::OK();
}

// ── 删除 ────────────────────────────────────────────────────

Status BPlusTree::Remove(const std::string& leaf_key, bool* removed) {
  *removed = false;
  if (!valid()) {
    return Status::Error(StatusCode::kInvalidArgument, "索引未初始化");
  }
  page_id_t leaf = kInvalidPageId;
  const Status s = FindLeaf(&leaf, leaf_key);
  if (!s.ok()) {
    return s;
  }
  PageGuard g(bpm_, bpm_->get_page(leaf));
  if (!g.valid()) {
    return Status::Error(StatusCode::kPageNotFound, "索引叶子页不存在");
  }
  IndexNode node(g.get());
  for (size_t i = 0; i < node.key_count(); ++i) {
    std::string k;
    if (!node.GetKey(i, &k)) {
      continue;
    }
    if (k == leaf_key) {
      // 墓碑式删除：槽位摘除，键数据空间不回收（分裂/合并时会紧凑化）
      // 说明：不实现节点合并（underflow merge）——教学规模下树高极小，
      // 删除后空间浪费不影响正确性；这是有意的实现简化，已写进文档。
      node.RemoveKeyAt(i);
      g.MarkDirty();
      *removed = true;
      return Status::OK();
    }
    if (CompareIndexKey(k, leaf_key, false) > 0) {
      break;   // 已超过目标位置，不存在
    }
  }
  return Status::OK();
}

// ── 扫描 ────────────────────────────────────────────────────

Status BPlusTree::ScanRange(const std::string* lo_column_key, const std::string* hi_column_key,
                            bool include_hi, const ScanCallback& cb) {
  if (!valid()) {
    return Status::OK();
  }
  // 定位起始叶子
  page_id_t leaf = kInvalidPageId;
  std::string probe;
  if (lo_column_key != nullptr) {
    probe = *lo_column_key;
  }
  const Status s = FindLeaf(&leaf, probe);
  if (!s.ok()) {
    return s;
  }

  page_id_t cur = leaf;
  bool started = (lo_column_key == nullptr);
  while (cur != kInvalidPageId) {
    PageGuard g(bpm_, bpm_->get_page(cur));
    if (!g.valid()) {
      return Status::Error(StatusCode::kPageNotFound, "索引叶子页不存在");
    }
    IndexNode node(g.get());
    const uint16_t n = node.key_count();
    for (size_t i = 0; i < n; ++i) {
      std::string k;
      if (!node.GetKey(i, &k)) {
        continue;
      }
      // 下界：与「列值键」比较（叶子键去掉行定位后才是同一形态）。
      // 比较时把边界键当作前缀，结果 >0 表示 k 的列值 > lo 的列值。
      if (!started && lo_column_key != nullptr) {
        if (CompareIndexKey(k, *lo_column_key, true) < 0) {
          continue;               // k 的列值 < lo → 跳过
        }
        started = true;
      }
      // 上界：同样按列值形态比较。
      //   c > 0                        → k 的列值 > hi 的列值 → 越界，结束
      //   c == 0 && !include_hi        → k 的列值 == hi 的列值，但不含上界 → 结束
      //   c == 0 && include_hi         → 含上界，继续输出
      // 注意 CompareIndexKey(..., true) 在「列值相同但 k 更长（带行定位）」时
      // 返回 ±1 而非 0，因此这里必须自己判断「列值是否相等」。
      if (hi_column_key != nullptr) {
        const std::string k_col = StripLeafRowId(k);
        const int c = k_col.compare(*hi_column_key);
        if (c > 0 || (c == 0 && !include_hi)) {
          return Status::OK();   // 越界 → 结束
        }
      }
      if (!cb(k)) {
        return Status::OK();     // 回调要求提前终止
      }
    }
    cur = node.right_sibling();
  }
  return Status::OK();
}

Status BPlusTree::Scan(ScanLeafCallback cb) {
  return ScanRange(nullptr, nullptr, true, std::move(cb));
}

// ── 前缀扫描（复合索引「最左前缀」原语）─────────────────────
// 实现：定位到 prefix 应落入的叶子（FindLeaf 直接吃列值键形态的 probe，
// 前缀本身就是合法的「部分元组编码」，下降比较按前缀语义进行），
// 然后沿叶子链表扫描：
//   * 列值部分 < prefix          → 还没到，跳过；
//   * 列值部分以 prefix 开头      → 命中，回调；
//   * 列值部分 > prefix 且非前缀  → 已越过前缀区（键有序，后面不会再有）→ 停。
// 判断只做**字节前缀比较**，完全不依赖解码 —— 因此对含任意字节
// （包括 0x00/0xFF）的 VARCHAR 也正确。
Status BPlusTree::ScanPrefix(const std::string& prefix, const ScanCallback& cb) {
  if (!valid() || prefix.empty()) {
    return Status::OK();
  }
  page_id_t leaf = kInvalidPageId;
  const Status s = FindLeaf(&leaf, prefix);
  if (!s.ok()) {
    return s;
  }
  page_id_t cur = leaf;
  while (cur != kInvalidPageId) {
    PageGuard g(bpm_, bpm_->get_page(cur));
    if (!g.valid()) {
      return Status::Error(StatusCode::kPageNotFound, "索引叶子页不存在");
    }
    IndexNode node(g.get());
    for (size_t i = 0; i < node.key_count(); ++i) {
      std::string k;
      if (!node.GetKey(i, &k)) {
        continue;
      }
      const std::string k_col = StripLeafRowId(k);
      if (k_col < prefix) {
        continue;                       // 还没到前缀区
      }
      if (k_col.size() < prefix.size() || k_col.compare(0, prefix.size(), prefix) != 0) {
        return Status::OK();            // 越过前缀区 → 结束
      }
      if (!cb(k)) {
        return Status::OK();            // 回调要求提前终止
      }
    }
    cur = node.right_sibling();
  }
  return Status::OK();
}

Status BPlusTree::ScanAll(std::vector<std::string>* out) {
  out->clear();
  return Scan([out](const std::string& k) {
    out->push_back(k);
    return true;
  });
}

// ── 统计信息（P1.6 代价模型用）─────────────────────────────
//
// Height：一直往「最左子指针」走，数到叶子为止。根是叶子时高度 = 1。
// 不走 LowerBound 是因为不需要键 —— 只要骨架层数，第 0 个子指针恒存在
// （内部节点至少有 1 个子指针，分裂逻辑保证）。
uint16_t BPlusTree::Height() const {
  if (!valid()) {
    return 0;
  }
  uint16_t h = 0;
  page_id_t cur = root_;
  for (;;) {
    PageGuard g(bpm_, bpm_->get_page(cur));
    if (!g.valid()) {
      return 0;  // 页缺失 → 拿不到可信高度
    }
    ++h;
    IndexNode node(g.get());
    if (node.type() == IndexNodeType::kLeaf) {
      return h;
    }
    const page_id_t next = node.Child(0);
    if (next == kInvalidPageId) {
      return 0;
    }
    cur = next;
    // 防御：结构异常时不要死循环（正常 B+ 树高度远小于此）
    if (h > 64) {
      return 0;
    }
  }
}

// LeafPageCount：从最左叶子沿 right_sibling 链表数一遍。
uint64_t BPlusTree::LeafPageCount() const {
  if (!valid()) {
    return 0;
  }
  // 先降到最左叶子（与 Height 同一段路径）
  page_id_t cur = root_;
  for (;;) {
    PageGuard g(bpm_, bpm_->get_page(cur));
    if (!g.valid()) {
      return 0;
    }
    IndexNode node(g.get());
    if (node.type() == IndexNodeType::kLeaf) {
      break;
    }
    const page_id_t next = node.Child(0);
    if (next == kInvalidPageId) {
      return 0;
    }
    cur = next;
  }
  uint64_t n = 0;
  while (cur != kInvalidPageId) {
    ++n;
    PageGuard g(bpm_, bpm_->get_page(cur));
    if (!g.valid()) {
      return n;  // 走到坏页就停在已数到的部分
    }
    IndexNode node(g.get());
    cur = node.right_sibling();
  }
  return n;
}

}  // namespace cella::storage
