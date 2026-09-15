// wal_types.h —— 预写日志（WAL）的记录类型与二进制编解码。
//
// 为什么需要它（对应 docs/GAP_ANALYSIS.md 阶段 P2）：
//   改造前 `journal.log` 只是**审计轨迹**——提交/回滚后才补一行文本，没有 LSN、
//   没有变更前后的像，重启时完全无法据此恢复。它是 INTEGRATION.md「已知取舍 #8」
//   里点名的纸面承诺：进程被强杀，缓冲池里的脏页全丢，已提交的事务也一起没了。
//
//   本模块把它升级为真正的 WAL：
//     * 每条记录带单调递增的 LSN（日志序列号，同时是「时间」）；
//     * 行变更记录带**前后像**（before / after），足以重做与撤销；
//     * 记录定长头里有 magic + 长度 + CRC32，崩溃造成的半截尾巴能被识别并丢弃。
//
// ── 为什么是「逻辑日志」而不是「物理页日志」──────────────────
//   存储层的 UPDATE 是「删旧行 + 插新行」，行没有稳定物理标识：
//       * Rid 会变（新行落在堆尾）；
//       * 槽位会被后续 INSERT 复用（陈旧 Rid 可能指向另一行）。
//   所以物理 redo（按 Rid 覆写字节）在这里根本无法做到幂等。
//   退一步选**行级逻辑日志**：记录「表 + 前后像」，重做/撤销按内容定位行。
//   这样 redo 天然幂等（有主键时按主键定位，精确；无主键时按整行内容匹配），
//   代价是：无主键表存在完全重复行时，重做只能保证「内容集合」正确。
//   这条边界与既有 undo（也是按内容补偿）一致，不是本次新引入的缺陷。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cella/db/common/db_status.h"
#include "cella/db/txn/lock_manager.h"
#include "cella/storage/common/types.h"
#include "cella/storage/common/value.h"

namespace cella::db::wal {

// ── 日志序列号 ──────────────────────────────────────────────
// 0 保留为「无效 LSN」；文件里第一条记录是 1。LSN 全局单调递增，
// 既标识记录本身，也标识「该记录所代表的数据库状态时刻」。
using lsn_t = uint64_t;
constexpr lsn_t kInvalidLsn = 0;
constexpr lsn_t kFirstLsn = 1;

// ── 记录类型 ────────────────────────────────────────────────
enum class RecordType : uint8_t {
  kBegin = 1,       // 事务开始
  kCommit = 2,      // 事务提交（提交点：此前本事务的全部记录必须已落盘）
  kAbort = 3,       // 事务结束且已回滚（含恢复期的回滚）
  kInsert = 4,      // 插入一行（after = 新行内容）
  kDelete = 5,      // 删除一行（before = 旧行内容）
  kUpdate = 6,      // 更新一行（before → after）
  kCheckpoint = 7,  // 存盘点：此后全部脏页已落盘，附带当时仍活动的事务表
};

const char* ToString(RecordType t);

// ── 一条日志记录（内存形态）────────────────────────────────
struct WalRecord {
  lsn_t lsn = kInvalidLsn;
  RecordType type = RecordType::kBegin;
  txn_id_t txn_id = kInvalidTxnId;

  // 行变更记录用：表名（原始拼写）
  std::string table;
  // 该行当时所在的物理页号。存储层没有暴露 pageLSN，所以页级 redo 判定用不上它；
  // 但它让恢复期能重建**脏页表（DPT）**，用于诊断与「恢复范围」的对外解释。
  storage::page_id_t page_id = storage::kInvalidPageId;
  std::vector<storage::Value> before;  // kDelete/kUpdate：变更前
  std::vector<storage::Value> after;   // kInsert/kUpdate：变更后

  // kCheckpoint：存盘瞬间仍活动的事务及其首条记录 LSN。
  // 恢复时 redo 起点就由它们决定（见 recovery_manager）。
  std::vector<std::pair<txn_id_t, lsn_t>> active_txns;

  bool is_data_record() const {
    return type == RecordType::kInsert || type == RecordType::kDelete ||
           type == RecordType::kUpdate;
  }
};

// 人类可读的一行描述（日志 / 诊断 / 测试失败信息）
std::string Describe(const WalRecord& r);

// ── 恢复期撤销的执行者（P2.5）───────────────────────────────
// 撤销 = 反向重放：撤销一次插入就是「按内容删掉那一行」，撤销一次删除就是
// 「按内容把那一行插回去」。语义与运行时的逻辑补偿完全一致，只是定位方式
// 换成了「按主键/内容」而不是按 Rid（WAL 里没有槽号，只有页号）。
// 由 Executor 实现（它才知道索引该怎么跟着维护）。
class IUndoApplier {
 public:
  virtual ~IUndoApplier() = default;
  // 撤销一次插入：row 是被插入的那一行
  virtual DbStatus UndoInsert(const std::string& table,
                              const std::vector<storage::Value>& row) = 0;
  // 撤销一次删除：row 是被删掉的旧内容
  virtual DbStatus UndoDelete(const std::string& table,
                              const std::vector<storage::Value>& row) = 0;
  // 撤销一次更新：把 new_row 换回 old_row
  virtual DbStatus UndoUpdate(const std::string& table,
                              const std::vector<storage::Value>& new_row,
                              const std::vector<storage::Value>& old_row) = 0;
};

// ── 二进制编解码 ────────────────────────────────────────────
// 落盘布局（小端）：
//   文件头 16B： "CELLAWAL1" + u32 version + u32 保留
//   每条记录：  magic u32 | size u32 | crc32 u32 | lsn u64 | payload...
//      size  = 整条记录字节数（含 20 字节头）
//      crc32 = 对 payload 计算的 CRC（IEEE 802.3，与常见的 crc32 实现一致）
//   读日志时遇到 magic 不符 / size 越界 / CRC 不匹配 → 视为「崩溃留下的半截记录」，
//   从这里截断，后面的一律不认。
constexpr uint32_t kRecordMagic = 0x314C4157u;  // "WAL1"
constexpr uint32_t kRecordHeaderSize = 20;
constexpr uint32_t kFileHeaderSize = 16;
constexpr uint32_t kFormatVersion = 1;

// payload（lsn 之后的部分）编解码。失败返回 false。
std::string EncodePayload(const WalRecord& r);
bool DecodePayload(RecordType type, const char* data, size_t size, WalRecord* out);

uint32_t Crc32(const void* data, size_t size);

// 值编码（解码在 wal_manager.cpp 内部完成，外部只需要 Encode/DecodePayload）
void PutValue(std::string* out, const storage::Value& v);

// 行内容比较（NULL 视为彼此相等，与索引层的 NULL 语义一致）
bool ValueEqual(const storage::Value& a, const storage::Value& b);
bool ValuesEqual(const std::vector<storage::Value>& a,
                 const std::vector<storage::Value>& b);

}  // namespace cella::db::wal
