#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cella/storage/common/byte_buffer.h"
#include "cella/storage/common/status.h"
#include "cella/storage/index/index_page_layout.h"
#include "cella/storage/page/page.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// IndexNode —— 把一个 Page 当作 B+ 树节点来读写（纯内存操作）。
//
// 统一布局（固定不变，插入键时不移动子指针数组，避免实现复杂度）：
//
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ PageHeader(32) │ NodeHeader(12) │ 子指针数组(4×(K+1)) │ 槽数组 │空闲│键│
//   └──────────────────────────────────────────────────────────────────┘
//   44          56               …                        free_end
//
//   * K = 该页允许的最大键数（构造时由 BPlusTree 按页大小算好并传入）。
//   * 子指针数组大小固定为 4×(K+1)，叶子节点也保留这段空间（不用），
//     换来「两种节点几何完全一致」——实现简单，而 4KB 页下这点浪费可接受。
//   * 槽项 4 字节 { key_off:u16, key_len:u16 }，键数据从页尾向左增长。
//   * 键编码为字节串（见 index_key.h），B+ 树只按字节字典序比较。
//
// 关键几何（全部由 K 决定，页内偏移固定）：
//   kChildBase = 44
//   kSlotBase  = 44 + 4×(K+1)
//   free_end   从 page_size 向下增长
// ─────────────────────────────────────────────────────────────────────────

class IndexNode {
 public:
  // max_keys 从节点头里读（节点自描述），因此不需要调用方传容量。
  explicit IndexNode(Page* page) : page_(page) {
    max_keys_ = GetUint16(page_->data() + kPageHeaderSize + index_node_header::kMaxKeys);
    slot_base_ = kIndexDataBegin + 4 * (static_cast<size_t>(max_keys_) + 1);
  }

  // ── 初始化（新页）─────────────────────────────────────────
  void Init(IndexNodeType t, uint16_t max_keys) {
    page_->SetHeaderPageType(PageType::kIndexPage);
    page_->SetSlotCount(0);
    page_->SetFreeEnd(static_cast<uint16_t>(page_->page_size()));
    SetMaxKeys(max_keys);
    max_keys_ = max_keys;
    slot_base_ = kIndexDataBegin + 4 * (static_cast<size_t>(max_keys) + 1);
    SetType(t);
    SetKeyCount(0);
    SetRightSibling(kInvalidPageId);
  }

  uint16_t max_keys() const { return max_keys_; }

  // ── 节点标识 ──────────────────────────────────────────────
  IndexNodeType type() const {
    const char* p = page_->data() + kPageHeaderSize;
    return static_cast<IndexNodeType>(static_cast<uint8_t>(p[index_node_header::kNodeType]));
  }
  void SetType(IndexNodeType t) {
    char* p = page_->data() + kPageHeaderSize;
    p[index_node_header::kNodeType] = static_cast<char>(static_cast<uint8_t>(t));
  }

  uint16_t key_count() const {
    return GetUint16(page_->data() + kPageHeaderSize + index_node_header::kKeyCount);
  }
  void SetKeyCount(uint16_t n) {
    PutUint16(page_->data() + kPageHeaderSize + index_node_header::kKeyCount, n);
  }

  page_id_t right_sibling() const {
    return GetUint32(page_->data() + kPageHeaderSize + index_node_header::kRightSibling);
  }
  void SetRightSibling(page_id_t id) {
    PutUint32(page_->data() + kPageHeaderSize + index_node_header::kRightSibling, id);
  }

  // 每页最大键数（节点自描述，写入节点头，供重建时读取）
  uint16_t MaxKeysField() const {
    return GetUint16(page_->data() + kPageHeaderSize + index_node_header::kMaxKeys);
  }
  void SetMaxKeys(uint16_t n) {
    PutUint16(page_->data() + kPageHeaderSize + index_node_header::kMaxKeys, n);
  }

  // ── 子指针（仅内部节点有效）───────────────────────────────
  // children[i] 是第 i 棵子树；i ∈ [0, key_count]
  page_id_t Child(size_t i) const {
    return GetUint32(page_->data() + kIndexDataBegin + i * 4);
  }
  void SetChild(size_t i, page_id_t id) {
    PutUint32(page_->data() + kIndexDataBegin + i * 4, id);
  }

  // ── 键存取 ────────────────────────────────────────────────
  bool GetKey(size_t slot, std::string* out) const {
    if (slot >= key_count()) {
      return false;
    }
    const char* s = SlotAt(slot);
    const uint16_t off = GetUint16(s);
    const uint16_t len = GetUint16(s + 2);
    if (len == 0) {
      return false;
    }
    out->assign(page_->data() + off, len);
    return true;
  }

  uint16_t KeyLen(size_t slot) const {
    if (slot >= key_count()) {
      return 0;
    }
    return GetUint16(SlotAt(slot) + 2);
  }

  // ── 插入一个键（不包含子指针维护；由 BPlusTree 负责）───────
  // at = 插入位置（键数组内的下标）；调用方保证空间足够。
  Status InsertKeyAt(size_t at, const std::string& key) {
    if (at > key_count()) {
      return Status::Error(StatusCode::kInvalidArgument, "插入位置越界");
    }
    if (key.empty() || key.size() > 0xFFFFu) {
      return Status::Error(StatusCode::kInvalidArgument, "索引键长度非法");
    }
    if (FreeSpace() < key.size() + 4) {
      return Status::Error(StatusCode::kPageFull, "索引页空间不足");
    }
    const uint16_t n = key_count();
    // 键数据区向左增长：free_end 前移 key.size()
    uint16_t free_end = page_->GetFreeEnd();
    const uint16_t off = static_cast<uint16_t>(free_end - key.size());
    std::memcpy(page_->data() + off, key.data(), key.size());
    page_->SetFreeEnd(off);

    // 槽数组在 at 处腾出一个 4 字节空位（后面的槽整体后移）
    char* base = SlotBasePtr();
    if (at < n) {
      std::memmove(base + (at + 1) * 4, base + at * 4, (static_cast<size_t>(n) - at) * 4);
    }
    PutUint16(base + at * 4, off);
    PutUint16(base + at * 4 + 2, static_cast<uint16_t>(key.size()));
    SetKeyCount(static_cast<uint16_t>(n + 1));
    return Status::OK();
  }

  // ── 删除指定位置的键（槽整体前移；不回收键数据空间）────────
  // 回收键数据需要紧凑化，代价高且易错；墓碑式删除即可（页会被分裂/重组）。
  void RemoveKeyAt(size_t at) {
    const uint16_t n = key_count();
    if (at >= n) {
      return;
    }
    char* base = SlotBasePtr();
    if (at + 1 < n) {
      std::memmove(base + at * 4, base + (at + 1) * 4, (static_cast<size_t>(n) - at - 1) * 4);
    }
    SetKeyCount(static_cast<uint16_t>(n - 1));
  }

  // ── 整体清空（重建时用）───────────────────────────────────
  void Clear() {
    page_->SetSlotCount(0);
    page_->SetFreeEnd(static_cast<uint16_t>(page_->page_size()));
    SetKeyCount(0);
    SetRightSibling(kInvalidPageId);
  }

  // ── 空间 ──────────────────────────────────────────────────
  // 当前可用字节：free_end 到「槽数组末尾」之间的距离。
  // 注意：新键还要占一个槽（4B），调用方需自行加 4。
  size_t FreeSpace() const {
    const uint16_t free_end = page_->GetFreeEnd();
    const size_t slot_end = slot_base_ + static_cast<size_t>(key_count()) * 4;
    if (free_end <= slot_end) {
      return 0;
    }
    return static_cast<size_t>(free_end) - slot_end;
  }

  Page* page_;
  uint16_t max_keys_ = 0;
  size_t slot_base_ = kIndexDataBegin;

 private:
  const char* SlotAt(size_t slot) const { return SlotBasePtr() + slot * 4; }
  char* SlotBasePtr() const { return page_->data() + slot_base_; }
};

// ── 每页最大键数估算 ─────────────────────────────────────────
// 内部节点：每键最坏占 (4 槽 + 4 指针) = 8B 骨架 + 键本体；
// 叶子节点：每键 4B 骨架 + 键本体 + 行定位 6B。
// 用「最大键长」保守估算，保证任何实际键都放得下。
inline uint16_t MaxKeysPerIndexPage(uint32_t page_size, size_t max_key_bytes, bool internal) {
  const size_t avail = static_cast<size_t>(page_size) - kIndexDataBegin -
                       4 * 1 /* 至少给一个子指针留位（内部）*/;
  size_t per = max_key_bytes + 4;                       // 槽 + 键
  if (internal) {
    per += 4;                                           // 子指针（按当前键数布局前的固定区不重复计）
  } else {
    per += kIndexRowIdSize;                             // 行定位
  }
  if (per == 0) {
    return 4;
  }
  size_t k = avail / per;
  if (k < 4) {
    k = 4;
  }
  if (k > 500) {
    k = 500;   // 上限保护（页内偏移是 u16）
  }
  return static_cast<uint16_t>(k);
}

}  // namespace cella::storage
