#include "cella/storage/table/table_heap.h"

#include <vector>

#include "cella/storage/buffer/buffer_pool_manager.h"
#include "cella/storage/buffer/page_guard.h"
#include "cella/storage/page/slotted_page_layout.h"
#include "cella/storage/record/i_record_serializer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// TableHeap：一张表 = 一条「数据页双向链表」。
//
//   首数据页 (first_page_id_)
//        │
//        ▼
//   ┌────────┐   next   ┌────────┐   next   ┌────────┐
//   │  page  │ ───────► │  page  │ ───────► │  page  │ ──► …(next=invalid)
//   │        │ ◄─────── │        │ ◄─────── │        │
//   └────────┘   prev   └────────┘   prev   └────────┘
//
// 页之间的链关系存在每页页头的 prev_page_id / next_page_id 字段里。
// 新纪录总是往「最后一页」塞；塞不下就新开一页挂到链表尾。
// ─────────────────────────────────────────────────────────────────────────

TableHeap::TableHeap(BufferPoolManager* bpm, page_id_t first_page_id, const Schema& schema,
                     IRecordSerializer* serializer)
    : bpm_(bpm), first_page_id_(first_page_id), schema_(schema), serializer_(serializer),
      fsm_(first_page_id) {}

Status TableHeap::InsertRecord(const Record& record, Rid* out) {
  // ① 把逻辑记录（Record）序列化成字节流
  std::vector<char> bytes;
  Status s = serializer_->Serialize(record, schema_, &bytes);
  if (!s.ok()) {
    return s;
  }
  const uint16_t len = static_cast<uint16_t>(bytes.size());

  // ② 从「插入目标页提示」开始（FreeSpaceManager 缓存了上次插入的页，
  //    避免每次都从首页走 O(n) 链表；但提示可能失效，下面会校正）
  page_id_t page_id = fsm_.hint_page();
  if (page_id == kInvalidPageId) {
    page_id = first_page_id_;
  }

  // ③ 跟随 next 链，走到「真正的最后一页」。
  //    为什么不能只信 hint：如果 hint 页已满且又被别人新开了页，hint 就落后了；
  //    这里一路 next 到底，保证接下来一定在链表尾操作，不会把链搞断。
  for (;;) {
    PageGuard pg(bpm_, bpm_->get_page(page_id));
    if (!pg.valid()) {
      return Status::Error(StatusCode::kIoError, "读取页失败");
    }
    const page_id_t next = pg->GetHeaderNextPageId();
    if (next == kInvalidPageId) {
      break;                          // 到链表尾了
    }
    page_id = next;
  }

  // ④ 尝试往最后一页塞；塞不下就新开一页，挂到链表尾再塞
  while (true) {
    PageGuard pg(bpm_, bpm_->get_page(page_id));
    if (!pg.valid()) {
      return Status::Error(StatusCode::kIoError, "读取页失败");
    }
    SlottedPageLayout layout(pg.get());
    slot_id_t slot = 0;
    s = layout.InsertRecord(bytes.data(), len, &slot);
    if (s.ok()) {
      pg.MarkDirty();                 // 页被改过，标记脏
      *out = Rid{page_id, slot};      // 记录定位：页号 + 槽号
      fsm_.update_hint(page_id);      // 记住这页，下次从这里开始
      if (row_count_known_) {
        ++row_count_;                 // 行数增量维护（未知态保持未知，等懒扫描）
      }
      return Status::OK();
    }
    if (s.code() != StatusCode::kPageFull) {
      return s;                       // 记录超长等错误，直接返回（换页也放不下）
    }

    // 页满：向缓冲池申请一个新页
    page_id_t new_page = kInvalidPageId;
    Page* np = bpm_->NewPage(&new_page);
    if (np == nullptr) {
      return Status::Error(StatusCode::kNoFreePage, "无法分配新页");
    }
    {
      // 初始化新页页头：prev 指向旧尾页，next 暂时置空（它自己就是新尾页）
      PageGuard ng(bpm_, np);
      ng->SetHeaderPageId(new_page);
      ng->SetHeaderPageType(PageType::kDataPage);
      ng->SetHeaderPrevPageId(page_id);
      ng->SetHeaderNextPageId(kInvalidPageId);
      ng.MarkDirty();
    }
    // 旧尾页的 next 指向新页（pg 此刻仍被 pin，可以直接改它的页头）
    pg->SetHeaderNextPageId(new_page);
    pg.MarkDirty();

    page_id = new_page;
    fsm_.update_hint(new_page);       // 提示更新为新尾页
  }
}

Status TableHeap::GetRecord(const Rid& rid, Record* out) {
  if (!rid.IsValid()) {
    return Status::Error(StatusCode::kInvalidArgument, "无效 Rid");
  }
  PageGuard pg(bpm_, bpm_->get_page(rid.page_id));
  if (!pg.valid()) {
    return Status::Error(StatusCode::kPageNotFound, "页不存在");
  }
  // 用槽式布局按槽号取出记录字节，再反序列化成 Record
  SlottedPageLayout layout(pg.get());
  const char* data = nullptr;
  uint16_t len = 0;
  Status s = layout.GetRecord(rid.slot_id, &data, &len);
  if (!s.ok()) {
    return s;
  }
  return serializer_->Deserialize(data, len, schema_, out);
}

Status TableHeap::DeleteRecord(const Rid& rid) {
  if (!rid.IsValid()) {
    return Status::Error(StatusCode::kInvalidArgument, "无效 Rid");
  }
  PageGuard pg(bpm_, bpm_->get_page(rid.page_id));
  if (!pg.valid()) {
    return Status::Error(StatusCode::kPageNotFound, "页不存在");
  }
  SlottedPageLayout layout(pg.get());
  // 先探活再删：对墓碑重复删时 DeleteRecord 语义不变（仍返回 OK），
  // 但行数只能对「真的活行」递减一次。
  const char* probe_data = nullptr;
  uint16_t probe_len = 0;
  const bool was_live = layout.GetRecord(rid.slot_id, &probe_data, &probe_len).ok();
  Status s = layout.DeleteRecord(rid.slot_id);   // 标记删除（槽 len 置 0）
  if (s.ok()) {
    pg.MarkDirty();
    if (row_count_known_ && was_live) {
      --row_count_;
    }
  }
  return s;
}

TableIterator TableHeap::begin() {
  // 从首页、槽 0 开始，构造时自动定位到第一条有效记录
  return TableIterator(bpm_, first_page_id_, schema_, serializer_);
}

TableIterator TableHeap::end() {
  return TableIterator();             // 默认构造 = end 哨兵
}

size_t TableHeap::RowCount() {
  if (!row_count_known_) {
    // 懒扫描：沿链表数一遍活行（TableIterator 自动跳过墓碑），
    // 之后由 InsertRecord/DeleteRecord 增量维护，本进程内 O(1)。
    size_t n = 0;
    for (auto it = begin(); it != end(); ++it) {
      ++n;
    }
    row_count_ = n;
    row_count_known_ = true;
  }
  return row_count_;
}

}  // namespace cella::storage
