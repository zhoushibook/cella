#pragma once
#include "types.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// FreeSpaceManager：记录「下一页记录往哪插」的提示页（hint）。
//
// 背景：往一张表插记录时，总是往「最后一页」塞。如果每次都从首页开始沿着
// 双向链表走到尾，插入是 O(页数)。有了 hint 页号，绝大多数插入直接从最后一页
// 开始，摊还 O(1)。
//
// 注意：hint 只是「提示」，可能因为各种原因失效（比如别处新开了页）。
// TableHeap 用它做起点，但会再沿 next 链校正到真正的尾页，保证正确性。
//
// 当前实现极简（只缓存一个页号）；将来可扩展成「按剩余空间定位」的复杂版本。
// ─────────────────────────────────────────────────────────────────────────
class FreeSpaceManager {
 public:
  FreeSpaceManager() = default;
  explicit FreeSpaceManager(page_id_t first_page) : hint_page_(first_page) {}

  page_id_t hint_page() const { return hint_page_; }
  void update_hint(page_id_t p) { hint_page_ = p; }   // 插入/开新页后更新

 private:
  page_id_t hint_page_ = kInvalidPageId;   // 当前建议的插入目标页
};

}  // namespace cella::storage
