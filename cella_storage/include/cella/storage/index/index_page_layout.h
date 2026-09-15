#pragma once
#include "cella/storage/page/page.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 索引页布局（B+ 树节点）—— §8 的索引页扩展。
//
// 与数据页（SlottedPageLayout）的区别：
//   * 数据页是「页内一排记录」，索引页是「页内一排有序键 + 子指针」；
//   * 索引页需要「节点类型」「当前键数」「右兄弟指针」「父指针」这些结构字段；
//   * 索引页的键是**变长**的（INT 4B、VARCHAR 可达 255B），因此槽目录必须
//     与数据页同构：头部 + 槽数组（向右增长）+ 键数据（向左增长）。
//
// 内存布局（一页 page_size 字节）：
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ PageHeader(32B, 复用) │ NodeHeader(12B) │ 槽数组 →│ 空闲 │← 键 │
//   └──────────────────────────────────────────────────────────────┘
//   低地址                                            free_end   高地址
//
//   NodeHeader 紧跟在 PageHeader 之后（偏移 32）：
//     32  node_type:u8    33  key_count:u16   35  (reserved:u8)
//     36  right_sibling:u32                    40  (reserved:u32)
//   共 12 字节 → 键区起点 = kPageHeaderSize + kIndexNodeHeaderSize = 44
//
//   槽项（每键 4 字节，与数据页一致）：
//     叶子：{ key_off:u16, key_len:u16 }                         —— 键 + 行定位
//     内部：{ key_off:u16, key_len:u16 } 后紧跟 u32 子页号
//           → 内部节点每键占 8 字节（4 槽 + 4 子指针，子指针放在槽数组之后）
//
//   为简单与正确起见，内部节点的子指针单独放在「槽数组之后、键区之前」的一段
//   连续数组里（共 MaxKeys+1 个 u32），槽数组只存键。
//
// 叶子节点语义（与 B+ 树标准一致）：
//   * 叶子键 = 索引列值 + 行定位 Rid（page_id:u32 + slot_id:u16），
//     这样同一键值可以对应多行（非唯一索引），且删除能精确定位到行；
//   * 叶子之间用 right_sibling 串成有序链表 → 范围扫描只需叶子顺序遍历。
//
// 内部节点语义：
//   * keys[i] 是第 i+1 棵子树的最小键（「分隔键」= 右子树最小键，标准 B+ 树约定）；
//   * children[i] 指向第 i 棵子树，children[key_count] 是最后一棵；
//   * 因此查找 key 时找「最后一个 <= key 的 keys[i]」，走 children[i]。
// ─────────────────────────────────────────────────────────────────────────

enum class IndexNodeType : uint8_t {
  kLeaf     = 0,
  kInternal = 1,
};

namespace index_node_header {
constexpr size_t kNodeType      = 0;   // u8
constexpr size_t kKeyCount      = 1;   // u16
constexpr size_t kMaxKeys       = 3;   // u16   每页最大键数（节点自描述）
constexpr size_t kRightSibling  = 5;   // u32
constexpr size_t kReserved      = 9;   // u16 补齐到 12 字节
}  // namespace index_node_header

constexpr size_t kIndexNodeHeaderSize = 12;
// 键区起点：页头 + 节点头
constexpr size_t kIndexDataBegin = kPageHeaderSize + kIndexNodeHeaderSize;

// ── 行定位（叶子里的行指针）─────────────────────────────────
// 6 字节打包：page_id:u32 + slot_id:u16（u16 与 slot_id_t 同宽，无 255 上限）
struct IndexRowId {
  page_id_t page_id = kInvalidPageId;
  uint8_t   slot_id = 0;

  bool IsValid() const { return page_id != kInvalidPageId; }
  bool operator==(const IndexRowId& o) const {
    return page_id == o.page_id && slot_id == o.slot_id;
  }
  bool operator!=(const IndexRowId& o) const { return !(*this == o); }
};

constexpr size_t kIndexRowIdSize = 6;   // page_id(4) + slot_id(2)
// 槽号为什么必须 2 字节：slot_id_t 是 uint16_t，页内槽数没有 255 的上限
// （实测 (INT,INT) 记录在 4KB 页上能放 270 行）。早先按 1 字节编码时，
// 槽号 ≥ 256 会被截断成 0..，与同页低槽号的键**逐字节相同**，插入时被
// 当成「完全重复键」静默丢弃 —— 表现为「表里明明有这行，按索引列点查却
// 返回 0 行」（每张表页最后 14 行查不到，见 docs/演示评估与改进建议.md D1）。

}  // namespace cella::storage
