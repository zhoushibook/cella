# EXTENSIBILITY.md — 六大扩展点

> 每个扩展点 = 抽象 + ≥1 实现，新增实现**不改核心代码**。

---

## 1. 替换策略（最重要）

**抽象** `Replacer`（`buffer/replacer.h`）：`Insert / Pin / Victim / Remove / Size / Name`。
**现有实现**：`LruReplacer`、`FifoReplacer`、`ClockReplacer`。

### 完整示例：新增一个「MRU」策略（最久使用淘汰）

**第 1 步**：新文件 `buffer/mru_replacer.h`

```cpp
#pragma once
#include "cella/storage/buffer/replacer.h"
#include <list>
#include <unordered_map>

namespace cella::storage {
// MRU：淘汰「最近使用」的帧（与 LRU 相反，演示用）
class MruReplacer : public Replacer {
 public:
  explicit MruReplacer(size_t /*n*/) {}
  void Insert(frame_id_t f) override {
    auto it = map_.find(f);
    if (it != map_.end()) list_.erase(it->second);
    list_.push_front(f);
    map_[f] = list_.begin();
  }
  bool Pin(frame_id_t f) override {
    auto it = map_.find(f);
    if (it == map_.end()) return false;
    list_.erase(it->second); map_.erase(it); return true;
  }
  bool Victim(frame_id_t* out) override {
    if (list_.empty()) return false;
    *out = list_.front();          // MRU：淘汰队头（最近使用）
    list_.pop_front(); map_.erase(*out); return true;
  }
  void Remove(frame_id_t f) override {
    auto it = map_.find(f);
    if (it != map_.end()) { list_.erase(it->second); map_.erase(it); }
  }
  size_t Size() const override { return list_.size(); }
  std::string Name() const override { return "MRU"; }
 private:
  std::list<frame_id_t> list_;
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> map_;
};
}  // namespace cella::storage
```

**第 2 步**：注册 + 使用

```cpp
#include "cella/storage/buffer/replacer_factory.h"
#include "mru_replacer.h"
...
ReplacerFactory::Register("MRU", [](size_t n){ return std::make_unique<MruReplacer>(n); });
auto replacer = ReplacerFactory::Create("MRU", pool_size);   // 与 LRU/FIFO 完全一致
```

**diff 核心**：`BufferPoolManager` **零改动**——它只依赖 `Replacer` 抽象 + 工厂，从不引用具体策略。

---

## 2. 磁盘后端 `IDiskManager`

实现 `Open/Close/ReadPage/WritePage/AllocatePage/FreePage` + 计数，新文件即可（如 `RemoteDiskManager` 走网络、`EncryptedDiskManager` 加解密）。

## 3. 存储实现 `IStorage`

新文件实现 `IStorage`（如 `MemStorage` 全内存、`EncryptedStorage` 加密），`CreateStorage` 里按需分发。

## 4. 页布局 `PageLayout`

实现 `InsertRecord/GetRecord/DeleteRecord/GetFreeSpace`，新文件即可（如 `PaxPageLayout`）。

## 5. 记录序列化 `IRecordSerializer`

实现 `Serialize/Deserialize`，新文件即可（如 `ColumnarSerializer` 列式）。

## 6. 数据类型 / 日志

- **数据类型**：`ValueType` 枚举集中定义于 `types.h`，序列化 switch 集中在 `slotted_record_serializer.cpp`，新增类型改这两处。
- **日志**：实现 `ILogger`（`Log/Flush`），新文件即可（如 `RemoteLogger` 上报网络）。

---

## 扩展性自证（CLOCK）

CLOCK 就是「新增替换策略」的实例：`clock_replacer.h/.cpp` 新文件 + `ReplacerFactory::Register("CLOCK", ...)`，`BufferPoolManager` 一行未改。运行 `./storage_demo` 第 ⑪ 步「换 CLOCK 再跑一遍」即可验证。测试 `clock_replacer_basic` 覆盖其行为。
