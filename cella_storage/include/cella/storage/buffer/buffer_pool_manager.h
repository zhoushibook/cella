#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cella/storage/common/buffer_stats.h"
#include "cella/storage/common/status.h"
#include "cella/storage/common/types.h"
#include "cella/storage/page/page.h"

namespace cella::storage {

class IDiskManager;
class Replacer;
class ILogger;

// ─────────────────────────────────────────────────────────────────────────
// 缓冲池（本模块的核心）
//
// 作用：在内存里缓存最近访问过的页，避免每次都读磁盘。
// 它维护一个「帧数组」——每个帧（Frame）是一个固定大小的内存槽，用来装一页数据。
//
// 三层数据关系：
//   frames_       帧数组，下标 = frame_id（0..pool_size-1）
//   page_table_   页号 → 帧号 的哈希表（"某页在哪个帧里"）
//   free_frames_  空闲帧栈（还没装任何页的帧）
//
// 淘汰（内存不够装新页时）：先拿空闲帧；没有空闲帧就让 Replacer 选一个
// 「牺牲帧」淘汰掉（脏页要先写回磁盘），再复用那个帧。
// ─────────────────────────────────────────────────────────────────────────
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, std::unique_ptr<IDiskManager> disk,
                    std::unique_ptr<Replacer> replacer);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  // ★ 取页并 pin（pin 计数 +1）。命中直接返回；未命中则从磁盘读入某个帧。
  //   找不到可用帧（全部被 pin）时返回 nullptr。
  Page*  get_page(page_id_t);
  // ★ 把某页写回磁盘，但仅在它是「脏页」时（干净页不产生磁盘写）。
  bool   flush_page(page_id_t);
  // 向磁盘申请一个全新的页号并装入帧，pin 计数 =1。调用方写完要 unpin。
  Page*  NewPage(page_id_t* out);
  // pin 计数 -1；is_dirty 表示调用方是否改写过这一页。
  // 计数归零时把该帧交给 Replacer（变为「可淘汰」）。
  bool   UnpinPage(page_id_t, bool is_dirty);
  // ★ 整页读入调用方缓冲（不 pin，也不改变缓冲池状态；缓存命中则直接拷贝）。
  Status read_page(page_id_t, char*);
  // ★ 整页写（不 pin；缓存命中则只更新缓存并标记脏，未命中则直接落盘）。
  Status write_page(page_id_t, const char*);
  // 把所有脏页写回磁盘（Close / 析构时调用）。
  void   FlushAllPages();
  const BufferStats& GetStats() const;

  void SetLogger(ILogger* logger) { logger_ = logger; }
  void LogStats();                             // 输出 [INFO][STATS] 行
  const std::deque<std::string>& recent_evictions() const { return eviction_log_; }

 private:
  // 一个帧 = 一个内存槽。装的是某页数据的副本。
  struct Frame {
    explicit Frame(uint32_t page_size) : page(page_size) {}
    page_id_t page_id = kInvalidPageId;  // 当前装的是哪一页（kInvalidPageId=空闲）
    int pin_count = 0;                   // pin 计数：>0 表示正被使用，不可淘汰
    bool is_dirty = false;               // 内存副本与磁盘是否不一致（改过没写回）
    Page page;                           // 页数据本体（vector<char>，可配置页大小）
  };

  // 找一个能用的帧：优先空闲帧，否则淘汰一个牺牲帧。找不到返回 false。
  bool find_free_frame(frame_id_t* out);
  // 记录一条替换日志（保留最近 10 条，并转发给 logger）。
  void record_eviction(frame_id_t frame, page_id_t page, bool dirty);

  std::vector<Frame> frames_;                       // 帧数组
  std::unordered_map<page_id_t, frame_id_t> page_table_;  // 页号 → 帧号
  std::vector<frame_id_t> free_frames_;             // 空闲帧栈（LIFO）
  std::unique_ptr<IDiskManager> disk_;              // 磁盘后端（唯一 I/O 出口）
  std::unique_ptr<Replacer> replacer_;              // 淘汰策略（LRU/FIFO/CLOCK…）
  BufferStats stats_;                               // 统计计数
  ILogger* logger_ = nullptr;
  std::deque<std::string> eviction_log_;            // 最近 10 条替换日志
};

}  // namespace cella::storage
