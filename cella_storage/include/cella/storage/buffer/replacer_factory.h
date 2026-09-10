#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "cella/storage/buffer/replacer.h"

namespace cella::storage {

// ── 替换策略工厂：注册 + 创建。新增策略零改动核心。───────────
class ReplacerFactory {
 public:
  using Creator = std::function<std::unique_ptr<Replacer>(size_t num_frames)>;

  static bool Register(const std::string& name, Creator creator);
  static std::unique_ptr<Replacer> Create(const std::string& name, size_t num_frames);
  static std::vector<std::string> RegisteredNames();
};

}  // namespace cella::storage
