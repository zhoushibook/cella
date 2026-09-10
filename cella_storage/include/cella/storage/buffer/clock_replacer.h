#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "cella/storage/buffer/replacer.h"

namespace cella::storage {

// CLOCK（演示扩展点）：环形时钟 + 引用位，近似 LRU。
// 新增策略 = 新文件 + ReplacerFactory::Register，核心零改动。
class ClockReplacer : public Replacer {
 public:
  explicit ClockReplacer(size_t num_frames)
      : ref_bits_(num_frames, false), in_clock_(num_frames, false) {}

  void Insert(frame_id_t f) override;
  bool Pin(frame_id_t f) override;
  bool Victim(frame_id_t* out) override;
  void Remove(frame_id_t f) override;
  size_t Size() const override { return size_; }
  std::string Name() const override { return "CLOCK"; }

 private:
  std::vector<bool> ref_bits_;   // 引用位
  std::vector<bool> in_clock_;   // 是否在时钟中
  size_t hand_ = 0;
  size_t size_ = 0;
};

}  // namespace cella::storage
