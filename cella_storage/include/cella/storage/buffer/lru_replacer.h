#pragma once
#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include "cella/storage/buffer/replacer.h"

namespace cella::storage {

// LRU：双向链表 + 哈希。头=最近使用，尾=最久未用；淘汰尾部。
class LruReplacer : public Replacer {
 public:
  explicit LruReplacer(size_t /*num_frames*/) {}

  void Insert(frame_id_t f) override;
  bool Pin(frame_id_t f) override;
  bool Victim(frame_id_t* out) override;
  void Remove(frame_id_t f) override;
  size_t Size() const override { return list_.size(); }
  std::string Name() const override { return "LRU"; }

 private:
  std::list<frame_id_t> list_;
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> map_;
};

}  // namespace cella::storage
