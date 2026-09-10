#include "cella/storage/disk/file_disk_manager.h"

#include <cstring>
#include <vector>

#include "cella/storage/common/byte_buffer.h"
#include "cella/storage/common/version.h"
#include "cella/storage/page/meta_page.h"
#include "cella/storage/page/page.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// FileDiskManager：把整个数据库存成一个文件。
//
//   文件就是一个「页数组」：第 page_id 页的字节偏移 = page_id * page_size。
//   page_id=0 是 MetaPage（元信息页），记录版本、页数、空闲链表头等。
//
// 空闲页链表（删除页后复用）：
//   - MetaPage.free_list_head 指向第一个空闲页；
//   - 每个空闲页的页头 next_page_id 指向下一个空闲页，串成一条链；
//   - AllocatePage 优先弹链表头复用，链空才在文件尾追加新页。
// ─────────────────────────────────────────────────────────────────────────

FileDiskManager::~FileDiskManager() { (void)Close(); }

Status FileDiskManager::Open(const std::string& path) {
  // 打开文件：已存在则读写打开；不存在则先建空文件再读写打开
  file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_.is_open()) {
    file_.clear();
    file_.open(path, std::ios::out | std::ios::binary);   // 创建空文件
    file_.close();
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
  }
  if (!file_.is_open()) {
    return Status::Error(StatusCode::kIoError, "无法打开文件: " + path);
  }

  // 判断文件是「全新的」还是「已有数据」：看文件大小
  file_.seekg(0, std::ios::end);
  const std::streamoff size = file_.tellg();
  file_.seekg(0, std::ios::beg);

  if (size == 0) {
    // 全新文件：写入一份初始化过的 MetaPage（只有 1 页，即 MetaPage 自身）
    page_count_ = 1;
    free_list_head_ = kInvalidPageId;
    return WriteMetaPage();
  }

  // 已有文件：读取并校验 MetaPage（含版本校验，旧版本文件在这里被拦下）
  return ReadMetaPage();
}

Status FileDiskManager::Close() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
  return Status::OK();
}

Status FileDiskManager::ReadPage(page_id_t page_id, char* data) {
  if (!file_.is_open()) {
    return Status::Error(StatusCode::kIoError, "文件未打开");
  }
  // 偏移公式：offset = page_id * page_size（用 seekg 移动读指针到对应位置）
  const std::streamoff off = static_cast<std::streamoff>(page_id) * page_size_;
  file_.seekg(off, std::ios::beg);
  file_.read(data, static_cast<std::streamsize>(page_size_));
  // 读不满一整页 = 这一页在文件里还不存在（没写过）
  if (file_.gcount() != static_cast<std::streamsize>(page_size_)) {
    return Status::Error(StatusCode::kPageNotFound, "读取页失败: " + std::to_string(page_id));
  }
  ++read_count_;
  return Status::OK();
}

Status FileDiskManager::WritePage(page_id_t page_id, const char* data) {
  if (!file_.is_open()) {
    return Status::Error(StatusCode::kIoError, "文件未打开");
  }
  const std::streamoff off = static_cast<std::streamoff>(page_id) * page_size_;
  file_.seekp(off, std::ios::beg);     // seekp：移动「写指针」（fstream 读写指针分离）
  file_.write(data, static_cast<std::streamsize>(page_size_));
  if (!file_) {
    return Status::Error(StatusCode::kIoError, "写入页失败: " + std::to_string(page_id));
  }
  file_.flush();                       // 立即刷到操作系统，保证 Close 前数据不丢
  ++write_count_;
  return Status::OK();
}

page_id_t FileDiskManager::AllocatePage() {
  page_id_t out = kInvalidPageId;
  if (free_list_head_ != kInvalidPageId) {
    // 有空闲页：复用链表头那一页。
    // 先读出它的页头 next_page_id（它指向的下一个空闲页），
    // 再把链表头指针前移一格，相当于「弹出」这一页。
    out = free_list_head_;
    std::vector<char> buf(page_size_, 0);
    if (!ReadPage(out, buf.data()).ok()) {
      return kInvalidPageId;
    }
    free_list_head_ = GetUint32(buf.data() + page_header::kNextPageId);
  } else {
    // 无空闲页：在文件尾追加一页（页号 = 当前总页数）
    out = page_count_;
    ++page_count_;
  }
  // 持久化 MetaPage（page_count / free_list_head 变了）
  if (!WriteMetaPage().ok()) {
    return kInvalidPageId;
  }
  return out;
}

Status FileDiskManager::FreePage(page_id_t page_id) {
  // 把这一页「头插」进空闲链表：页头 next_page_id 指向原链表头，
  // 同时把页类型标记为 FreeListPage（仅作调试标记用）。
  std::vector<char> buf(page_size_, 0);
  PutUint32(buf.data() + page_header::kNextPageId, free_list_head_);
  PutUint16(buf.data() + page_header::kPageType, static_cast<uint16_t>(PageType::kFreeListPage));
  const Status s = WritePage(page_id, buf.data());
  if (!s.ok()) {
    return s;
  }
  free_list_head_ = page_id;           // 链表头指向刚释放的这一页
  return WriteMetaPage();
}

Status FileDiskManager::WriteMetaPage() {
  // 把内存里的元信息（版本/页大小/页数/空闲链表头/目录根页）打包成 32 字节写回 page 0
  MetaPage meta;
  meta.format_version    = kFormatVersion;
  meta.page_size         = page_size_;
  meta.page_count        = page_count_;
  meta.free_list_head    = free_list_head_;
  meta.catalog_root_page = catalog_root_page_;
  meta.checksum          = 0;

  std::vector<char> buf(page_size_, 0);
  meta.Encode(buf.data());
  return WritePage(kMetaPageId, buf.data());
}

Status FileDiskManager::ReadMetaPage() {
  // 读 page 0，解码 + 校验
  std::vector<char> buf(page_size_, 0);
  const Status s = ReadPage(kMetaPageId, buf.data());
  if (!s.ok()) {
    return s;
  }
  MetaPage meta;
  const Status d = MetaPage::Decode(buf.data(), &meta);   // 校验 magic
  if (!d.ok()) {
    return d;
  }
  if (meta.format_version != kFormatVersion) {
    // 旧版本数据文件 → 立刻定位，而不是表现为随机崩溃（约束 #2.2 约定 3）
    return Status::Error(StatusCode::kVersionMismatch, "数据文件格式版本不兼容");
  }
  if (meta.page_size != page_size_) {
    return Status::Error(StatusCode::kInvalidConfig, "page_size 与数据文件不一致");
  }
  page_count_         = meta.page_count;
  free_list_head_     = meta.free_list_head;
  catalog_root_page_  = meta.catalog_root_page;
  return Status::OK();
}

Status FileDiskManager::SetCatalogRootPage(page_id_t p) {
  catalog_root_page_ = p;
  return WriteMetaPage();
}

}  // namespace cella::storage
