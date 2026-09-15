#include "cella/db/txn/transaction.h"

#include <algorithm>
#include <sstream>

#include "cella/db/common/db_logger.h"
#include "cella/db/common/time_util.h"

namespace cella::db
{

  const char *ToString(TxnState s)
  {
    switch (s)
    {
    case TxnState::kActive:
      return "ACTIVE";
    case TxnState::kCommitted:
      return "COMMITTED";
    case TxnState::kAborted:
      return "ABORTED";
    case TxnState::kRollbackFailed:
      return "ROLLBACK_FAILED";
    }
    return "?";
  }

  // ── Transaction ─────────────────────────────────────────────

  Transaction::Transaction(txn_id_t id) : id_(id), begin_time_(NowEpochSeconds()) {}

  std::string Transaction::Describe() const
  {
    std::ostringstream os;
    os << "txn=" << id_ << " 状态=" << ToString(state_) << " 语句数=" << statements_
       << " undo=" << undo_.size() << " 涉及表=[";
    bool first = true;
    for (const auto &t : touched_)
    {
      os << (first ? "" : ",") << t;
      first = false;
    }
    os << "]";
    return os.str();
  }

  // ── TxnManager ──────────────────────────────────────────────

  TxnManager::TxnManager(storage::IStorage *storage, LockManager *locks,
                         std::recursive_mutex *storage_mutex)
      : storage_(storage), locks_(locks), storage_mutex_(storage_mutex) {}

  TxnManager::~TxnManager() = default;

  // 把一次「逻辑补偿」写进 WAL。
  //
  // 为什么必须写：语句级回滚之后事务还会继续跑、最后可能提交。若补偿不入日志，
  // 崩溃重放时会先把那次 INSERT 重做出来，却不知道它后来被撤掉了 —— 凭空多一行。
  // 补偿记录就是教科书里 CLR 的等价物，只是它以普通数据记录的形态入日志
  // （撤销一次插入 = 一条 kDelete，其 before 是被撤销行的 after）。
  void TxnManager::LogCompensationLocked(txn_id_t id, const UndoRecord &u)
  {
    if (wal_ == nullptr)
    {
      return;
    }
    wal::WalRecord r;
    r.txn_id = id;
    r.table = u.table;
    r.page_id = u.rid.page_id;
    switch (u.kind)
    {
    case UndoRecord::Kind::kInsert:
      r.type = wal::RecordType::kDelete;
      r.before = u.after.values();
      break;
    case UndoRecord::Kind::kDelete:
      r.type = wal::RecordType::kInsert;
      r.after = u.before.values();
      break;
    case UndoRecord::Kind::kUpdate:
      r.type = wal::RecordType::kUpdate;
      r.before = u.after.values();
      r.after = u.before.values();
      break;
    }
    (void)wal_->Append(r);
  }

  txn_id_t TxnManager::Begin()
  {
    std::shared_ptr<Transaction> txn;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      const txn_id_t id = ++next_id_;
      txn = std::make_shared<Transaction>(id);
      txns_[id] = txn;
    }
    if (wal_ != nullptr)
    {
      wal::WalRecord r;
      r.type = wal::RecordType::kBegin;
      r.txn_id = txn->id();
      txn->SetFirstLsn(wal_->Append(r));
    }
    DbLogInfo(logcat::kTxn, "BEGIN txn=" + std::to_string(txn->id()));
    return txn->id();
  }

  std::shared_ptr<Transaction> TxnManager::Find(txn_id_t id) const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    const auto it = txns_.find(id);
    return it == txns_.end() ? nullptr : it->second;
  }

  size_t TxnManager::active_count() const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    size_t n = 0;
    for (const auto &kv : txns_)
    {
      if (kv.second->active())
      {
        ++n;
      }
    }
    return n;
  }

  size_t TxnManager::committed_total() const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    return committed_total_;
  }

  size_t TxnManager::aborted_total() const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    return aborted_total_;
  }

  uint64_t TxnManager::next_txn_id() const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    return next_id_;
  }

  size_t TxnManager::UndoMark(txn_id_t id) const
  {
    std::shared_ptr<Transaction> txn;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      const auto it = txns_.find(id);
      if (it == txns_.end())
      {
        return 0;
      }
      txn = it->second;
    }
    return txn->undo_count();
  }

  DbStatus TxnManager::RollbackToMark(txn_id_t id, size_t mark, const std::string &reason)
  {
    std::shared_ptr<Transaction> txn;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      const auto it = txns_.find(id);
      if (it == txns_.end())
      {
        return DbStatus::Error(DbCode::kInternal, "回滚未知事务 txn=" + std::to_string(id));
      }
      txn = it->second;
    }
    if (!txn->active())
    {
      return DbStatus::Ok();
    }
    // TxnManager 按 undo 水位截断日志，实现语句级原子性
    const DbStatus s = ApplyUndoLocked(txn.get(), mark);
    if (s.ok())
    {
      txn->TruncateUndo(mark);
      DbLogInfo(logcat::kTxn, "语句级回滚 txn=" + std::to_string(id) + " 至 undo 水位 " +
                                  std::to_string(mark) + "（" + reason + "）");
    }
    return s;
  }

  DbStatus TxnManager::Commit(txn_id_t id)
  {
    std::shared_ptr<Transaction> txn;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      const auto it = txns_.find(id);
      if (it == txns_.end())
      {
        return DbStatus::Error(DbCode::kInternal, "提交未知事务 txn=" + std::to_string(id));
      }
      txn = it->second;
    }
    if (!txn->active())
    {
      return DbStatus::Error(DbCode::kTxnAborted,
                             "事务已结束，无法提交: txn=" + std::to_string(id));
    }

    // 提交路径：锁在最后统一释放（严格 2PL）
    txn->SetState(TxnState::kCommitted);
    txn->SetEndTime(NowEpochSeconds());
    locks_->ReleaseAll(id);

    // ── WAL 规则：提交记录必须先落盘，才能向调用方报告成功 ──
    // 否则「COMMIT 返回 OK 后进程被杀」会丢掉这个事务 —— 那 WAL 就白写了。
    if (wal_ != nullptr)
    {
      wal::WalRecord r;
      r.type = wal::RecordType::kCommit;
      r.txn_id = id;
      (void)wal_->Append(r);
      const DbStatus ws = wal_->FlushDurable();
      if (!ws.ok())
      {
        return ws;
      }
    }

    std::ostringstream os;
    os << "COMMIT txn=" << id << " 语句=" << txn->statement_count()
       << " undo=" << txn->undo_count() << " 表=[";
    bool first = true;
    for (const auto &t : txn->touched_tables())
    {
      os << (first ? "" : ",") << t;
      first = false;
    }
    os << "]";
    {
      std::unique_lock<std::mutex> lk(mutex_);
      ++committed_total_;
    }
    DbLogInfo(logcat::kTxn, os.str());
    return DbStatus::Ok();
  }

  // 逆序补偿直到 stop_at（不含）；存储访问需要串行化。
  // stop_at == 0 即整事务回滚；stop_at == 语句前水位即语句级回滚。
  DbStatus TxnManager::ApplyUndoLocked(Transaction *t, size_t stop_at)
  {
    std::lock_guard<std::recursive_mutex> guard(*storage_mutex_);
    const std::vector<UndoRecord> &log = t->undo_log();
    const size_t limit = std::min(stop_at, log.size());
    size_t failures = 0;
    for (size_t i = log.size(); i-- > limit;)
    {
      const UndoRecord &u = log[i];
      storage::Status st;
      switch (u.kind)
      {
      case UndoRecord::Kind::kInsert:
        st = storage_->delete_record(u.table, u.rid);
        if (st.ok())
        {
          if (undo_hooks_ != nullptr)
          {
            // 新插入的行被撤销 → 它的索引项也必须撤掉，否则索引里留下指向空洞的键
            undo_hooks_->OnUndoInsertDeleted(u.table, u.rid);
          }
          LogCompensationLocked(t->id(), u);
        }
        break;
      case UndoRecord::Kind::kDelete:
      case UndoRecord::Kind::kUpdate:
      {
        if (u.kind == UndoRecord::Kind::kUpdate && u.rid.IsValid())
        {
          const storage::Status d = storage_->delete_record(u.table, u.rid);
          if (!d.ok())
          {
            DbLogWarn(logcat::kTxn, "回滚删除新版本失败: " + d.ToString());
          }
        }
        storage::Rid ignored;
        st = storage_->insert_record(u.table, u.before, &ignored);
        if (st.ok())
        {
          if (undo_hooks_ != nullptr)
          {
            // 旧内容按**新**物理位置复插：索引项必须按新 Rid 重建（旧键已随新版本删除）
            undo_hooks_->OnUndoRowRestored(u.table, ignored, u.before);
          }
          LogCompensationLocked(t->id(), u);
        }
        break;
      }
      }
      if (!st.ok())
      {
        ++failures;
        DbLogWarn(logcat::kTxn, "undo 项执行失败: " + st.ToString());
      }
    }
    if (failures != 0)
    {
      return DbStatus::Error(DbCode::kStorageError,
                             "回滚完成但存在 " + std::to_string(failures) + " 个 undo 项失败");
    }
    return DbStatus::Ok();
  }

  DbStatus TxnManager::RollbackLocked(Transaction *t) { return ApplyUndoLocked(t, 0); }

  DbStatus TxnManager::Abort(txn_id_t id, const std::string &reason)
  {
    std::shared_ptr<Transaction> txn;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      const auto it = txns_.find(id);
      if (it == txns_.end())
      {
        return DbStatus::Error(DbCode::kInternal, "回滚未知事务 txn=" + std::to_string(id));
      }
      txn = it->second;
    }
    if (!txn->active() && !txn->rollback_failed())
    {
      return DbStatus::Ok(); // 已结束：幂等
    }

    const DbStatus rb = RollbackLocked(txn.get());
    if (rb.ok())
    {
      txn->SetState(TxnState::kAborted);
      txn->SetEndTime(NowEpochSeconds());
      locks_->ReleaseAll(id);
    }
    else
    {
      txn->SetState(TxnState::kRollbackFailed);
    }

    std::ostringstream os;
    os << "ABORT txn=" << id << " undo=" << txn->undo_count() << " 原因=" << reason;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      if (rb.ok())
      {
        ++aborted_total_;
      }
    }
    // 回滚成功也要记 ABORT：恢复时据此知道「这个事务已经了结」，不必再撤销一次。
    if (wal_ != nullptr)
    {
      wal::WalRecord r;
      r.type = wal::RecordType::kAbort;
      r.txn_id = id;
      (void)wal_->Append(r);
      (void)wal_->FlushDurable();
    }
    DbLogWarn(logcat::kTxn, os.str());
    return rb;
  }

  std::vector<std::pair<txn_id_t, wal::lsn_t>> TxnManager::ActiveTxnsWithFirstLsn() const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    std::vector<std::pair<txn_id_t, wal::lsn_t>> out;
    for (const auto &kv : txns_)
    {
      if (kv.second->active())
      {
        out.emplace_back(kv.first, kv.second->first_lsn());
      }
    }
    return out;
  }

  void TxnManager::ObserveTxnId(txn_id_t id)
  {
    std::unique_lock<std::mutex> lk(mutex_);
    if (id > next_id_)
    {
      next_id_ = id;
    }
  }

  // 恢复期回滚（P2.5）。
  //
  // 为什么不用运行时的 ApplyUndoLocked：那条路径靠 UndoRecord 里的 **Rid** 定位行，
  // 而 WAL 记录里只有页号（槽号在崩溃重启后既不稳定也可能已被复用）。
  // 因此这里改为「反向重放」：撤销一条 kInsert 就是按内容删掉那一行，
  // 撤销一条 kDelete 就是按内容插回去 —— 与运行时补偿是同一套语义，只是定位方式
  // 换成主键/整行内容。applier（执行器）顺带把索引也维护了。
  DbStatus TxnManager::UndoWalRecords(txn_id_t id, const std::vector<wal::WalRecord> &records,
                                      wal::IUndoApplier *applier, const std::string &reason)
  {
    ObserveTxnId(id);
    size_t failures = 0;
    for (size_t i = records.size(); i-- > 0;)
    {
      const wal::WalRecord &r = records[i];
      DbStatus s;
      switch (r.type)
      {
      case wal::RecordType::kInsert:
        s = applier->UndoInsert(r.table, r.after);
        break;
      case wal::RecordType::kDelete:
        s = applier->UndoDelete(r.table, r.before);
        break;
      case wal::RecordType::kUpdate:
        s = applier->UndoUpdate(r.table, r.after, r.before);
        break;
      default:
        continue;
      }
      if (!s.ok())
      {
        ++failures;
        DbLogWarn(logcat::kTxn, "恢复期撤销失败 txn=" + std::to_string(id) + " " + s.ToString());
      }
    }
    locks_->ReleaseAll(id); // 恢复期锁表是空的，调用只为保持状态自洽
    {
      std::unique_lock<std::mutex> lk(mutex_);
      if (failures == 0)
      {
        ++aborted_total_;
      }
    }
    if (wal_ != nullptr)
    {
      wal::WalRecord end;
      end.type = wal::RecordType::kAbort;
      end.txn_id = id;
      (void)wal_->Append(end);
      (void)wal_->FlushDurable();
    }
    DbLogWarn(logcat::kTxn, "恢复期回滚 txn=" + std::to_string(id) + " 撤销=" +
                                std::to_string(records.size()) + " 失败=" +
                                std::to_string(failures) + " 原因=" + reason);
    if (failures != 0)
    {
      return DbStatus::Error(DbCode::kWalError,
                             "恢复期回滚 txn=" + std::to_string(id) + " 有 " +
                                 std::to_string(failures) + " 条撤销失败");
    }
    return DbStatus::Ok();
  }

  std::string TxnManager::Dump() const
  {
    std::unique_lock<std::mutex> lk(mutex_);
    size_t active = 0;
    for (const auto &kv : txns_)
    {
      if (kv.second->active())
      {
        ++active;
      }
    }
    std::ostringstream os;
    os << "事务表（共 " << txns_.size() << " 个，活动 " << active << "）:\n";
    for (const auto &kv : txns_)
    {
      os << "  " << kv.second->Describe() << "\n";
    }
    os << "已提交 " << committed_total_ << " / 已回滚 " << aborted_total_ << "\n";
    return os.str();
  }

} // namespace cella::db
