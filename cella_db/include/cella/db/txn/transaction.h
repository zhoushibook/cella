// transaction.h —— 事务对象与事务管理器（原子性 / 持久化审计）。
//
// 组成：
//   Transaction    一个事务的状态、回滚日志（逻辑 undo）、触碰过的表。
//   TxnManager     分配事务号、提交、回滚、写审计日志（journal.log）。
//
// 原子性如何实现（务实方案，见 docs/INTEGRATION.md「已知取舍」）：
//   存储层只提供 insert（追加）与 delete（置墓碑），且 delete 后无法按原
//   Rid 复活，因此 undo 采用「逻辑补偿」而非物理页回滚：
//     kInsert  → 回滚时把新插入的行标记删除
//     kDelete  → 回滚时按内容重新插入（逻辑内容恢复，物理位置可能变化）
//     kUpdate  → 回滚时删掉新版本、重插旧版本
//   对本教学系统的可观察语义（行内容）而言，回滚是精确的；物理 Rid 不保证复原。
//
// 持久化：提交时把该事务的变更通过存储层缓冲池留在内存，由存储层 Close 时的
//   FlushAllPages 落盘；journal.log 记录 COMMIT/ABORT 审计轨迹，用于诊断与
//   崩溃后的人工核对。这不是完整的 ARIES/WAL 恢复，属有意简化。
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cella/db/common/db_status.h"
#include "cella/db/txn/lock_manager.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/record.h"

namespace cella::db {

enum class TxnState { kActive, kCommitted, kAborted };
const char* ToString(TxnState s);

// ── 回滚日志项 ──────────────────────────────────────────────
struct UndoRecord {
  enum class Kind { kInsert, kDelete, kUpdate };
  Kind kind = Kind::kInsert;
  std::string table;
  storage::Rid rid;        // kInsert: 新行位置；kDelete/kUpdate: 变更后的行位置
  storage::Record before;  // kDelete/kUpdate: 变更前的内容
};

// ── 事务对象（单线程使用；跨线程共享由 TxnManager 负责）─────
class Transaction {
 public:
  explicit Transaction(txn_id_t id);

  txn_id_t id() const { return id_; }
  TxnState state() const { return state_; }
  void SetState(TxnState s) { state_ = s; }
  bool active() const { return state_ == TxnState::kActive; }

  void AddUndo(UndoRecord r) { undo_.push_back(std::move(r)); }
  const std::vector<UndoRecord>& undo_log() const { return undo_; }
  size_t undo_count() const { return undo_.size(); }
  // 语句级回滚后把日志截断到水位（配合 TxnManager::RollbackToMark）
  void TruncateUndo(size_t mark) {
    if (mark < undo_.size()) {
      undo_.resize(mark);
    }
  }

  void AddStatement() { ++statements_; }
  size_t statement_count() const { return statements_; }

  void TouchTable(const std::string& t) { touched_.insert(t); }
  const std::set<std::string>& touched_tables() const { return touched_; }

  int64_t begin_time() const { return begin_time_; }
  int64_t end_time() const { return end_time_; }
  void SetEndTime(int64_t t) { end_time_ = t; }

  // 诊断用一行文本
  std::string Describe() const;

 private:
  txn_id_t id_;
  TxnState state_ = TxnState::kActive;
  std::vector<UndoRecord> undo_;
  std::set<std::string> touched_;
  size_t statements_ = 0;
  int64_t begin_time_ = 0;
  int64_t end_time_ = 0;
};

// ── 事务管理器 ──────────────────────────────────────────────
// storage_mutex：存储层非线程安全，回滚时要回写数据，故需与执行层共用同一把
// 递归互斥量（由 DbEngine 持有并传入）。
class TxnManager {
 public:
  TxnManager(storage::IStorage* storage, LockManager* locks, std::recursive_mutex* storage_mutex);
  ~TxnManager();
  TxnManager(const TxnManager&) = delete;
  TxnManager& operator=(const TxnManager&) = delete;

  // 审计日志路径；空字符串表示不落文件
  void SetJournalPath(const std::string& path);
  const std::string& journal_path() const { return journal_path_; }

  txn_id_t Begin();
  DbStatus Commit(txn_id_t id);
  DbStatus Abort(txn_id_t id, const std::string& reason);

  // ── 语句级原子性 ──
  // 取当前 undo 水位；语句失败时回滚到该水位（只撤本语句的改动，不结束事务、
  // 不释放锁），从而避免「一条 INSERT 写了 3 行、第 2 行报错」留下半截数据。
  size_t UndoMark(txn_id_t id) const;
  DbStatus RollbackToMark(txn_id_t id, size_t mark, const std::string& reason);

  // 取事务句柄（共享所有权，保证使用期间不被销毁）
  std::shared_ptr<Transaction> Find(txn_id_t id) const;

  size_t active_count() const;
  size_t committed_total() const;
  size_t aborted_total() const;
  uint64_t next_txn_id() const;
  std::string Dump() const;

 private:
  DbStatus RollbackLocked(Transaction* t);              // 整事务回滚（需已持 storage_mutex）
  DbStatus ApplyUndoLocked(Transaction* t, size_t stop_at);  // 回滚到 undo 水位
  void WriteJournal(const std::string& line);

  storage::IStorage* storage_;
  LockManager* locks_;
  std::recursive_mutex* storage_mutex_;
  mutable std::mutex mutex_;  // 保护 txns_ / 计数器
  std::map<txn_id_t, std::shared_ptr<Transaction>> txns_;
  txn_id_t next_id_ = 0;
  size_t committed_total_ = 0;
  size_t aborted_total_ = 0;
  std::string journal_path_;
  std::ofstream journal_;
};

}  // namespace cella::db
