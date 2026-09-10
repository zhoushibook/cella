#include "cella/storage/buffer/clock_replacer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// CLOCK（时钟 / 二次机会）：近似 LRU 的经典算法，用于演示「如何新增策略」。
//
// 数据结构：两个位数组（长度 = 帧数）+ 一个时钟指针 hand_。
//   in_clock_[f] 帧 f 当前是否可淘汰
//   ref_bits_[f] 帧 f 的「引用位」：1 = 最近被访问过，再给它一次机会
//
// Victim 让时钟指针一圈圈转，遇到：
//   ref=1 → 把引用位清 0（给第二次机会），继续转
//   ref=0 → 淘汰它
// 相比 LRU 的精确排序，CLOCK 只用两个位数组，更省内存，近似 LRU 效果。
// ─────────────────────────────────────────────────────────────────────────

void ClockReplacer::Insert(frame_id_t f) {
  if (static_cast<size_t>(f) >= in_clock_.size()) {
    return;
  }
  if (!in_clock_[f]) {
    in_clock_[f] = true;
    ref_bits_[f] = true;   // 刚加入给一次机会（避免一进来就被淘汰）
    ++size_;
  }
}

bool ClockReplacer::Pin(frame_id_t f) {
  if (static_cast<size_t>(f) >= in_clock_.size()) {
    return false;
  }
  if (in_clock_[f]) {
    in_clock_[f] = false;   // 被 pin，移出时钟
    ref_bits_[f] = false;
    --size_;
    return true;
  }
  return false;
}

bool ClockReplacer::Victim(frame_id_t* out) {
  if (size_ == 0) {
    return false;
  }
  const size_t n = in_clock_.size();
  while (true) {
    // 时钟指针走到哪一格
    const frame_id_t f = static_cast<frame_id_t>(hand_);
    hand_ = (hand_ + 1) % n;         // 指针前移一圈（循环）
    if (!in_clock_[f]) {
      continue;                      // 不在时钟里，跳过
    }
    if (ref_bits_[f]) {
      ref_bits_[f] = false;          // 最近访问过 → 给第二次机会，清位继续转
      continue;
    }
    // 引用位为 0 → 淘汰
    in_clock_[f] = false;
    --size_;
    *out = f;
    return true;
  }
}

void ClockReplacer::Remove(frame_id_t f) {
  if (static_cast<size_t>(f) < in_clock_.size() && in_clock_[f]) {
    in_clock_[f] = false;
    ref_bits_[f] = false;
    --size_;
  }
}

}  // namespace cella::storage
