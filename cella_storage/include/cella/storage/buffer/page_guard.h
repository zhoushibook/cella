#pragma once
#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/page/page.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// PageGuard：RAII 版「pin/unpin 配对」（约束 #6）。
//
// 背景：get_page / NewPage 返回的页是「已 pin」的（pin 计数 +1），用完后必须
//       unpin，否则帧永远占着、缓冲池迟早耗尽（kNoFreeFrame）。
// 手动配对容易漏（比如函数中途 return），PageGuard 靠析构函数保证：
//      对象离开作用域时自动调 UnpinPage，无论正常退出还是提前 return。
//
// 用法：
//   PageGuard guard(&bpm, bpm.get_page(pid));   // 包住已 pin 的页
//   if (!guard.valid()) { ... }                  // 页可能拿不到（nullptr）
//   guard->data()[0] = 'x';                      // 像用 Page* 一样用 operator->
//   guard.MarkDirty();                           // 改过要标记脏（析构时 is_dirty=true 传回）
//   // 离开作用域 → 析构 → 自动 unpin
//
// 禁止拷贝（否则会重复 unpin）；也不支持移动（保持语义简单）。
// ─────────────────────────────────────────────────────────────────────────
class PageGuard {
 public:
  PageGuard(BufferPoolManager* bpm, Page* page) : bpm_(bpm), page_(page) {}
  ~PageGuard() {
    if (page_ != nullptr && bpm_ != nullptr) {
      bpm_->UnpinPage(page_->page_id(), is_dirty_);   // 自动归还这一页
    }
  }

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  Page* operator->() const { return page_; }   // 像指针一样访问页
  Page& operator*() const { return *page_; }
  Page* get() const { return page_; }
  bool valid() const { return page_ != nullptr; }
  void MarkDirty() { is_dirty_ = true; }        // 标记这一页被改过

 private:
  BufferPoolManager* bpm_;   // 持有缓冲池指针，析构时调用它的 UnpinPage
  Page* page_;               // 被保护的页（可能为 nullptr，表示没拿到页）
  bool is_dirty_ = false;    // 期间是否改写过（析构时作为 is_dirty 传回）
};

}  // namespace cella::storage
