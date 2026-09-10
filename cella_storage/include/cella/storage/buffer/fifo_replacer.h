#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_set>
#include "cella/storage/buffer/replacer.h"

namespace cella::storage {

// FIFO：队列 + 哈希。淘汰最先插入的帧（与 LRU 的「最近访问」不同——
// 重新访问不会改变其插入顺序，两者在相同负载下可产生不同淘汰结果）。
class FifoReplacer : public Replacer {
 public:
  explicit FifoReplacer(size_t /*num_frames*/) {}

  void Insert(frame_id_t f) override;
  bool Pin(frame_id_t f) override;
  bool Victim(frame_id_t* out) override;
  void Remove(frame_id_t f) override;
  size_t Size() const override { return evictable_.size(); }
  std::string Name() const override { return "FIFO"; }

 private:
  std::deque<frame_id_t> queue_;          // 插入顺序
  std::unordered_set<frame_id_t> in_queue_;   // 是否曾插入
  std::unordered_set<frame_id_t> evictable_;  // 当前可淘汰
};

}  // namespace cella::storage
