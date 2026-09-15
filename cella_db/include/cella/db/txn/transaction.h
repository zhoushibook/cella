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
// 持久化（P2 之后，见 docs/WAL_RECOVERY.md）：
//   journal.log 已从「提交后才补一行的审计文本」升级为真正的 WAL：
//     * 每条记录带 LSN；begin / commit / abort / checkpoint 由本管理器写入；
//     * 行变更（insert / delete / update）由执行器在改动发生的当下写入，
//       带前后像（before / after），足以重做也能撤销；
//     * undo 的每一步补偿同样入日志 —— 否则「语句级回滚后又提交」的事务
//       在重放时会把已补偿掉的改动又加回来。
//   WAL 规则由 DbEngine::Checkpoint 与 TxnManager::Commit 共同保证：
//   提交返回前本事务日志必须落盘；存盘点先刷日志再刷脏页。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cella/db/common/db_status.h"
#include "cella/db/txn/lock_manager.h"
#include "cella/db/wal/wal_manager.h"
#include "cella/db/wal/wal_types.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/record.h"

namespace cella::db
{

  enum class TxnState
  {
    kActive,
    kCommitted,
    kAborted,
    kRollbackFailed
  };
  const char *ToString(TxnState s);

  // ── 回滚日志项 ──────────────────────────────────────────────
  // after 是 P2 才补上的字段：WAL 的补偿记录需要「被撤销的那一行长什么样」
  // （撤销一次插入 = 删掉刚插入的内容 → 要写一条 kDelete，其 before 就是这里的 after）。
  // 没有它，撤销动作就无法完整入日志，重放时已补偿的改动会被加回来。
  struct UndoRecord
  {
    enum class Kind
    {
      kInsert,
      kDelete,
      kUpdate
    };
    Kind kind = Kind::kInsert;
    std::string table;
    storage::Rid rid;       // kInsert: 新行位置；kDelete/kUpdate: 变更后的行位置
    storage::Record before; // kDelete/kUpdate: 变更前的内容
    storage::Record after;  // kInsert/kUpdate: 变更后的内容
  };

  // ── 事务对象（单线程使用；跨线程共享由 TxnManager 负责）─────
  class Transaction
  {
  public:
    explicit Transaction(txn_id_t id);

    txn_id_t id() const { return id_; }
    TxnState state() const { return state_; }
    void SetState(TxnState s) { state_ = s; }
    bool active() const { return state_ == TxnState::kActive; }
    bool rollback_failed() const { return state_ == TxnState::kRollbackFailed; }

    UndoRecord &AddUndo(UndoRecord r)
    {
      undo_.push_back(std::move(r));
      return undo_.back();
    }
    const std::vector<UndoRecord> &undo_log() const { return undo_; }
    size_t undo_count() const { return undo_.size(); }
    // 语句级回滚后把日志截断到水位（配合 TxnManager::RollbackToMark）
    void TruncateUndo(size_t mark)
    {
      if (mark < undo_.size())
      {
        undo_.resize(mark);
      }
    }

    void AddStatement() { ++statements_; }
    size_t statement_count() const { return statements_; }

    void TouchTable(const std::string &t) { touched_.insert(t); }
    const std::set<std::string> &touched_tables() const { return touched_; }

    int64_t begin_time() const { return begin_time_; }
    int64_t end_time() const { return end_time_; }
    void SetEndTime(int64_t t) { end_time_ = t; }

    // ── WAL 相关（P2）───────────────────────────────────────
    // 本事务第一条日志记录的 LSN。存盘点要把它写进 checkpoint 记录，
    // 恢复时据此确定 redo 起点（仍活动的事务必须从头重放）。
    wal::lsn_t first_lsn() const { return first_lsn_; }
    void SetFirstLsn(wal::lsn_t l) { first_lsn_ = l; }

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
    wal::lsn_t first_lsn_ = wal::kInvalidLsn;
  };

  // ── 事务管理器 ──────────────────────────────────────────────
  // storage_mutex：存储层非线程安全，回滚时要回写数据，故需与执行层共用同一把
  // 递归互斥量（由 DbEngine 持有并传入）。
  class TxnManager
  {
  public:
    // ── undo 补偿的外挂钩子（P1.5）──────────────────────────
    // 表行与二级索引是两份派生关系的数据：回滚只补偿表行会让索引与表数据
    // 重新分叉。为了让「索引维护」的归属留在执行层（它才知道目录与索引定义），
    // 这里提供一个纯虚接口，由 DbEngine 注入执行器侧的实现：
    //   OnUndoInsertDeleted(table, rid)          —— 补偿撤销了新插入的行 → 索引项也要删
    //   OnUndoRowRestored(table, rid, record)    —— 补偿恢复了旧内容 → 索引项按新 Rid 重建
    // 两个钩子都在 storage_mutex 临界区内被调用。
    class UndoIndexHooks
    {
    public:
      virtual ~UndoIndexHooks() = default;
      virtual void OnUndoInsertDeleted(const std::string &table, const storage::Rid &rid) = 0;
      virtual void OnUndoRowRestored(const std::string &table, const storage::Rid &rid,
                                     const storage::Record &record) = 0;
    };
    void SetUndoIndexHooks(UndoIndexHooks *hooks) { undo_hooks_ = hooks; }

    TxnManager(storage::IStorage *storage, LockManager *locks, std::recursive_mutex *storage_mutex);
    ~TxnManager();
    TxnManager(const TxnManager &) = delete;
    TxnManager &operator=(const TxnManager &) = delete;

    // ── WAL 绑定（P2）────────────────────────────────────────
    // 由 DbEngine 在装配时注入；为空表示不记日志（也就没有崩溃恢复能力）。
    void SetWal(wal::WalManager *wal) { wal_ = wal; }
    wal::WalManager *wal() const { return wal_; }

    txn_id_t Begin();
    DbStatus Commit(txn_id_t id);
    DbStatus Abort(txn_id_t id, const std::string &reason);

    // ── 语句级原子性 ──
    // 取当前 undo 水位；语句失败时回滚到该水位（只撤本语句的改动，不结束事务、
    // 不释放锁），从而避免「一条 INSERT 写了 3 行、第 2 行报错」留下半截数据。
    size_t UndoMark(txn_id_t id) const;
    DbStatus RollbackToMark(txn_id_t id, size_t mark, const std::string &reason);

    // 取事务句柄（共享所有权，保证使用期间不被销毁）
    std::shared_ptr<Transaction> Find(txn_id_t id) const;

    // ── 供存盘点 / 恢复使用 ──────────────────────────────────
    // 仍活动的事务（id + 首条记录 LSN）：存盘点把它写进 checkpoint 记录，
    // 恢复阶段据此定 redo 起点。
    std::vector<std::pair<txn_id_t, wal::lsn_t>> ActiveTxnsWithFirstLsn() const;
    // 恢复期把日志里出现过的最大事务号告知本管理器，保证之后新分配的事务号
    // 不会与「刚被恢复回滚掉的事务」重号。
    void ObserveTxnId(txn_id_t id);
    // 恢复期回滚（P2.5）：records 是**日志顺序**（旧 → 新）的 WAL 行变更记录，
    // 本方法从后往前倒着撤销 —— 后做的先撤，这才是 undo 的方向。
    // 撤销动作用 applier 完成（= 执行器的反向重放），语义与运行时的逻辑补偿一致。
    // 结束后写一条 ABORT 并刷盘，这样下次恢复不必再撤一次。
    DbStatus UndoWalRecords(txn_id_t id, const std::vector<wal::WalRecord> &records,
                            wal::IUndoApplier *applier, const std::string &reason);

    size_t active_count() const;
    size_t committed_total() const;
    size_t aborted_total() const;
    uint64_t next_txn_id() const;
    std::string Dump() const;

  private:
    DbStatus RollbackLocked(Transaction *t);                  // 整事务回滚（需已持 storage_mutex）
    DbStatus ApplyUndoLocked(Transaction *t, size_t stop_at); // 回滚到 undo 水位
    // 把一次「逻辑补偿」写进 WAL。没有它，重放会把已撤销的改动又加回来。
    void LogCompensationLocked(txn_id_t id, const UndoRecord &u);

    storage::IStorage *storage_;
    LockManager *locks_;
    std::recursive_mutex *storage_mutex_;
    wal::WalManager *wal_ = nullptr;   // 不接管所有权
    mutable std::mutex mutex_; // 保护 txns_ / 计数器
    std::map<txn_id_t, std::shared_ptr<Transaction>> txns_;
    txn_id_t next_id_ = 0;
    size_t committed_total_ = 0;
    size_t aborted_total_ = 0;
    UndoIndexHooks *undo_hooks_ = nullptr; // 可为空（不维护索引 / 单元测试直连）
  };

} // namespace cella::db
