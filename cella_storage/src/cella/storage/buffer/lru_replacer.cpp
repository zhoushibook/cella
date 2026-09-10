#include "cella/storage/buffer/lru_replacer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// LRU（最近最少使用）：淘汰「最久没被访问」的帧。
//
// 数据结构：双向链表 list_（头 = 最近使用，尾 = 最久未用）+ 哈希 map_（帧号→链表位置）。
// 所有操作 O(1)。
// ─────────────────────────────────────────────────────────────────────────

void LruReplacer::Insert(frame_id_t f) {
  // 帧刚被 unpin，重新变成「可淘汰」。把它放到链表头 = 最近使用。
  auto it = map_.find(f);
  if (it != map_.end()) {
    list_.erase(it->second);   // 若已存在先移除，避免重复
  }
  list_.push_front(f);
  map_[f] = list_.begin();
}

bool LruReplacer::Pin(frame_id_t f) {
  // 帧被 pin（正在使用），从「可淘汰」集合里移除。
  auto it = map_.find(f);
  if (it == map_.end()) {
    return false;
  }
  list_.erase(it->second);
  map_.erase(it);
  return true;
}

bool LruReplacer::Victim(frame_id_t* out) {
  // 淘汰链表尾 = 最久没被访问的那一帧。
  if (list_.empty()) {
    return false;
  }
  const frame_id_t f = list_.back();
  list_.pop_back();
  map_.erase(f);
  *out = f;
  return true;
}

void LruReplacer::Remove(frame_id_t f) {
  // 帧被彻底移除（不复用了），从链表里删掉。
  auto it = map_.find(f);
  if (it != map_.end()) {
    list_.erase(it->second);
    map_.erase(it);
  }
}

}  // namespace cella::storage
