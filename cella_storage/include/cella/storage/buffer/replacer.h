#pragma once
#include <cstddef>
#include <string>
#include "cella/storage/common/types.h"

namespace cella::storage {

// ── 淘汰策略抽象：只管「哪些帧可淘汰」，不感知 pin 计数/脏页（由 BPM 负责）──
// 新增策略 = 新文件实现本接口 + ReplacerFactory::Register，BufferPoolManager 零改动。
class Replacer {
 public:
  virtual ~Replacer() = default;

  virtual void Insert(frame_id_t) = 0;       // 变为可淘汰（pin 归零时调用）
  virtual bool Pin(frame_id_t) = 0;          // 移出淘汰候选；返回是否在其中
  virtual bool Victim(frame_id_t* out) = 0;  // 选牺牲帧；无可用返回 false
  virtual void Remove(frame_id_t) = 0;       // 帧被复用/释放时移除
  virtual size_t Size() const = 0;
  virtual std::string Name() const = 0;
};

}  // namespace cella::storage
