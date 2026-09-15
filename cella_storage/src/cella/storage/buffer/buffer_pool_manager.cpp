#include "cella/storage/buffer/buffer_pool_manager.h"

#include <cstring>
#include <sstream>
#include <utility>

#include "cella/storage/buffer/replacer.h"
#include "cella/storage/common/logger.h"
#include "cella/storage/disk/i_disk_manager.h"

namespace cella::storage
{

  // 构造：按配置的 pool_size 预分配帧，全部帧先放进空闲栈。
  BufferPoolManager::BufferPoolManager(size_t pool_size, std::unique_ptr<IDiskManager> disk,
                                       std::unique_ptr<Replacer> replacer)
      : disk_(std::move(disk)), replacer_(std::move(replacer))
  {
    const uint32_t page_size = disk_->GetPageSize(); // 帧里的 Page 要按页大小分配内存
    frames_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i)
    {
      frames_.emplace_back(page_size);
      free_frames_.push_back(static_cast<frame_id_t>(i));
    }
  }

  BufferPoolManager::~BufferPoolManager() { FlushAllPages(); }

  Page *BufferPoolManager::get_page(page_id_t page_id)
  {
    ++stats_.access;

    // ── 命中：该页已经在缓冲池里 ──
    auto it = page_table_.find(page_id);
    if (it != page_table_.end())
    {
      Frame &f = frames_[it->second];
      ++f.pin_count;              // 使用中，pin 计数 +1
      replacer_->Pin(it->second); // 移出「可淘汰」集合（被 pin 的帧不能淘汰）
      ++stats_.hit;
      return &f.page; // 直接返回内存里的页，不再碰磁盘
    }

    // ── 未命中：需要从磁盘读入 ──
    ++stats_.miss;

    // 1. 找一个可用帧（空闲帧，或淘汰一个牺牲帧）
    frame_id_t frame;
    if (!find_free_frame(&frame))
    {
      return nullptr; // 所有帧都被 pin，没有可淘汰的帧（对应 kNoFreeFrame）
    }

    // 2. 把磁盘上的页读进这个帧
    Frame &f = frames_[frame];
    const Status s = disk_->ReadPage(page_id, f.page.data());
    if (!s.ok())
    {
      free_frames_.push_back(frame); // 读失败，把帧还给空闲栈
      return nullptr;
    }
    ++stats_.disk_reads;

    // 3. 登记这个帧现在装的是 page_id 这一页
    f.page_id = page_id;
    f.page.set_page_id(page_id);
    f.pin_count = 1;
    f.is_dirty = false;
    page_table_[page_id] = frame;
    return &f.page;
  }

  bool BufferPoolManager::flush_page(page_id_t page_id)
  {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end())
    {
      return false; // 页不在缓冲池，无从 flush
    }
    Frame &f = frames_[it->second];
    if (!f.is_dirty)
    {
      return true; // 干净页无需写盘（关键：不产生多余磁盘写）
    }
    const Status s = disk_->WritePage(page_id, f.page.data());
    if (!s.ok())
    {
      return false;
    }
    f.is_dirty = false; // 写回后清除脏标记
    ++stats_.disk_writes;
    return true;
  }

  Page *BufferPoolManager::NewPage(page_id_t *out)
  {
    // 1. 找一个可用帧（复用空闲帧或淘汰一个牺牲帧）
    frame_id_t frame;
    if (!find_free_frame(&frame))
    {
      *out = kInvalidPageId;
      return nullptr;
    }

    // 2. 向磁盘申请一个全新页号（磁盘层会优先复用空闲页链表）
    const page_id_t new_id = disk_->AllocatePage();
    if (new_id == kInvalidPageId)
    {
      free_frames_.push_back(frame); // 申请失败，归还帧
      *out = kInvalidPageId;
      return nullptr;
    }

    // 3. 清空该帧内容，登记为刚分配的新页（初始不脏——调用方写完再标记）
    Frame &f = frames_[frame];
    f.page_id = new_id;
    f.page.set_page_id(new_id);
    f.page.reset_memory();
    f.pin_count = 1;
    f.is_dirty = false;
    page_table_[new_id] = frame;
    ++stats_.page_allocs;
    *out = new_id;
    return &f.page;
  }

  bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty)
  {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end())
    {
      return false;
    }
    Frame &f = frames_[it->second];
    if (f.pin_count <= 0)
    {
      return false; // 已经归零，多余的 unpin 是调用方错误
    }
    --f.pin_count;
    if (is_dirty)
    {
      f.is_dirty = true; // 调用方改过这一页
    }
    if (f.pin_count == 0)
    {
      // 关键不变量：pin 归零的帧才进入 Replacer（成为可淘汰候选）
      replacer_->Insert(it->second);
    }
    return true;
  }

  Status BufferPoolManager::read_page(page_id_t page_id, char *buf)
  {
    // 缓存命中：直接拷贝内存副本，省一次磁盘读
    auto it = page_table_.find(page_id);
    if (it != page_table_.end())
    {
      const Frame &f = frames_[it->second];
      std::memcpy(buf, f.page.data(), f.page.page_size());
      return Status::OK();
    }
    // 未命中：直接读磁盘（不 pin、不装帧）
    const Status s = disk_->ReadPage(page_id, buf);
    if (s.ok())
    {
      ++stats_.disk_reads;
    }
    return s;
  }

  Status BufferPoolManager::write_page(page_id_t page_id, const char *buf)
  {
    // 缓存命中：更新内存副本 + 标记脏（真正写盘推迟到 flush / 淘汰 / Close）
    auto it = page_table_.find(page_id);
    if (it != page_table_.end())
    {
      Frame &f = frames_[it->second];
      std::memcpy(f.page.data(), buf, f.page.page_size());
      f.is_dirty = true;
      return Status::OK();
    }
    // 未命中：直接写磁盘
    const Status s = disk_->WritePage(page_id, buf);
    if (s.ok())
    {
      ++stats_.disk_writes;
    }
    return s;
  }

  void BufferPoolManager::FlushAllPages()
  {
    // 遍历所有在缓冲池里的页，把脏页写回磁盘（Close/析构时保证数据落盘）
    for (const auto &kv : page_table_)
    {
      Frame &f = frames_[kv.second];
      if (f.is_dirty)
      {
        const Status s = disk_->WritePage(kv.first, f.page.data());
        if (s.ok())
        {
          f.is_dirty = false;
          ++stats_.disk_writes;
        }
      }
    }
  }

  const BufferStats &BufferPoolManager::GetStats() const { return stats_; }

  void BufferPoolManager::LogStats()
  {
    if (logger_ != nullptr)
    {
      logger_->Log(LogLevel::kInfo, "STATS   ", stats_.ToLogLine());
    }
  }

  // 找一个可用的帧。两步走：
  //   ① 有空闲帧 → 直接弹一个（O(1)，无任何磁盘 I/O）
  //   ② 无空闲帧 → 让 Replacer 选一个「牺牲帧」，若它脏则先写回磁盘，再复用
  bool BufferPoolManager::find_free_frame(frame_id_t *out)
  {
    if (!free_frames_.empty())
    {
      *out = free_frames_.back();
      free_frames_.pop_back();
      return true;
    }

    // Replacer 里只装「pin 归零」的帧；Victim 失败 = 所有帧都被 pin 了
    frame_id_t victim;
    if (!replacer_->Victim(&victim))
    {
      return false;
    }

    Frame &f = frames_[victim];
    const bool dirty = f.is_dirty;
    const page_id_t old_page = f.page_id;

    if (dirty)
    {
      // 脏页必须先写回，否则淘汰后数据就丢了
      const Status s = disk_->WritePage(old_page, f.page.data());
      if (!s.ok())
      {
        return false;
      }
      ++stats_.disk_writes;
      ++stats_.dirty_flush;
      f.is_dirty = false;
    }

    // 把旧页从「页号→帧号」映射里摘掉，清空帧，交给调用方复用
    ++stats_.evict;
    page_table_.erase(old_page);
    f.page.reset_memory();
    record_eviction(victim, old_page, dirty);
    *out = victim;
    return true;
  }

  void BufferPoolManager::record_eviction(frame_id_t frame, page_id_t page, bool dirty)
  {
    // 组装替换日志：policy=LRU victim_frame=2 victim_page=5 dirty=true -> flushed
    std::ostringstream os;
    os << "policy=" << replacer_->Name() << " victim_frame=" << frame
       << " victim_page=" << page << " dirty=" << (dirty ? "true" : "false");
    if (dirty)
    {
      os << " -> flushed";
    }
    const std::string line = os.str();

    // 只保留最近 10 条（环形缓冲，供 demo 打印「最近替换日志」）
    eviction_log_.push_back(line);
    if (eviction_log_.size() > 10)
    {
      eviction_log_.pop_front();
    }
    if (logger_ != nullptr)
    {
      // 明细级（默认不落盘）：一次大扫描 / 一次恢复可能淘汰几十万次，按 WARN
      // 记录会把日志淹没 —— 实测一次卡死的恢复往存储日志里灌了 236MB。
      // 需要看替换轨迹时设 CELLA_LOG_LEVEL=debug 打开（见 common/logger.h）。
      // 供 demo / 诊断读取的最近 10 条仍在 eviction_log_ 里，不受级别影响。
      logger_->Log(LogLevel::kDebug, "REPLACER", line);
    }
  }

} // namespace cella::storage
