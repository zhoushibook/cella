#include "cella/storage/buffer/fifo_replacer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// FIFO（先进先出）：淘汰「最先被加载进来」的帧，不看访问顺序。
//
// 关键设计（为什么不能只用一条队列）：
//   - 如果 Pin 时把帧从队列移除、Insert 时再塞回队尾，那么「重新访问」就会
//     把帧挪到队尾，行为会退化成和 LRU 一样（都按「最近访问」排序）。
//   - 要体现「FIFO 与 LRU 真的不同」，必须让 Pin 只标记「不可淘汰」而**不改
//     插入顺序**。于是用三个结构：
//       queue_     插入顺序（只增不减，直到被淘汰）
//       in_queue_  是否在 queue_ 里（去重用）
//       evictable_ 当前是否可淘汰（Pin 置 false、Insert 置 true）
//
// Victim 从队头开始找第一个「仍可淘汰」的帧 —— 保证按最先插入的顺序淘汰。
// ─────────────────────────────────────────────────────────────────────────

void FifoReplacer::Insert(frame_id_t f) {
  if (evictable_.count(f) != 0) {
    return;                        // 已经可淘汰，什么都不做
  }
  evictable_.insert(f);            // 标记为可淘汰
  if (in_queue_.count(f) == 0) {
    // 只有「第一次」Insert 才记录顺序；重新 Insert（unpin）不改原顺序
    in_queue_.insert(f);
    queue_.push_back(f);
  }
}

bool FifoReplacer::Pin(frame_id_t f) {
  // 帧被 pin：只从「可淘汰」集合移除，保留它在 queue_ 里的位置
  auto it = evictable_.find(f);
  if (it == evictable_.end()) {
    return false;
  }
  evictable_.erase(it);
  return true;
}

bool FifoReplacer::Victim(frame_id_t* out) {
  // 从队头（最先插入）开始，找第一个仍可淘汰的帧（跳过被 pin 的）
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (evictable_.count(*it) != 0) {
      const frame_id_t f = *it;
      evictable_.erase(f);
      in_queue_.erase(f);
      queue_.erase(it);
      *out = f;
      return true;
    }
  }
  return false;
}

void FifoReplacer::Remove(frame_id_t f) {
  evictable_.erase(f);
  if (in_queue_.count(f) != 0) {
    in_queue_.erase(f);
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if (*it == f) {
        queue_.erase(it);
        break;
      }
    }
  }
}

}  // namespace cella::storage
