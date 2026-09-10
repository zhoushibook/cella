#include "cella/storage/buffer/replacer_factory.h"

#include <map>
#include <utility>

#include "cella/storage/buffer/clock_replacer.h"
#include "cella/storage/buffer/fifo_replacer.h"
#include "cella/storage/buffer/lru_replacer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 替换策略工厂：名字 → 构造函数的注册表。
//
// 它让「新增淘汰策略」变得零侵入：
//   缓冲池只依赖抽象 Replacer + 这里的 Create(name)，从不 include 具体策略。
//   加新策略 = 新文件实现 Replacer + Register 一行，核心代码一字不改。
//
// 为什么用「懒注册」（EnsureDefaults）而不是每个策略文件里的静态全局对象：
//   静态库里的静态全局对象若没被引用，链接器会把它连同注册代码一起丢弃，
//   导致策略没注册上。懒注册保证第一次 Create 时必定把内置策略装好。
// ─────────────────────────────────────────────────────────────────────────

namespace {

std::map<std::string, ReplacerFactory::Creator>& Registry() {
  static std::map<std::string, ReplacerFactory::Creator> r;   // 函数内 static：线程安全且懒初始化
  return r;
}

// 内置三种策略（LRU/FIFO 是主力，CLOCK 演示扩展点）
void EnsureDefaults() {
  static bool done = false;   // 只装一次
  if (done) {
    return;
  }
  done = true;
  ReplacerFactory::Register("LRU",
      [](size_t n) { return std::make_unique<LruReplacer>(n); });
  ReplacerFactory::Register("FIFO",
      [](size_t n) { return std::make_unique<FifoReplacer>(n); });
  ReplacerFactory::Register("CLOCK",
      [](size_t n) { return std::make_unique<ClockReplacer>(n); });
}

}  // namespace

// 注册一个策略；返回 false 表示该名字已存在（emplace 不覆盖）
bool ReplacerFactory::Register(const std::string& name, Creator creator) {
  return Registry().emplace(name, std::move(creator)).second;
}

// 按名字创建策略；未注册返回 nullptr
std::unique_ptr<Replacer> ReplacerFactory::Create(const std::string& name, size_t num_frames) {
  EnsureDefaults();
  auto it = Registry().find(name);
  if (it == Registry().end()) {
    return nullptr;
  }
  return it->second(num_frames);   // 调用对应的构造 lambda
}

std::vector<std::string> ReplacerFactory::RegisteredNames() {
  EnsureDefaults();
  std::vector<std::string> names;
  names.reserve(Registry().size());
  for (const auto& kv : Registry()) {
    names.push_back(kv.first);
  }
  return names;
}

}  // namespace cella::storage
