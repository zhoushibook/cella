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

  TxnManager::~TxnManager()
  {
    if (journal_.is_open())
    {
      journal_.flush();
      journal_.close();
    }
  }

  void TxnManager::SetJournalPath(const std::string &path)
  {
    std::unique_lock<std::mutex> lk(mutex_);
    journal_path_ = path;
    if (journal_.is_open())
    {
      journal_.close();
    }
    if (!path.empty())
    {
      journal_.open(path, std::ios::binary | std::ios::app);
      if (!journal_)
      {
        DbLogWarn(logcat::kTxn, "无法打开审计日志: " + path);
      }
    }
  }

  void TxnManager::WriteJournal(const std::string &line)
  {
    if (!journal_.is_open())
    {
      return;
    }
    journal_ << NowIso() << " " << line << "\n";
    journal_.flush();
  }

  txn_id_t TxnManager::Begin()
  {
    std::unique_lock<std::mutex> lk(mutex_);
    const txn_id_t id = ++next_id_;
    auto txn = std::make_shared<Transaction>(id);
    txns_[id] = txn;
    DbLogInfo(logcat::kTxn, "BEGIN txn=" + std::to_string(id));
    return id;
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
      WriteJournal(os.str());
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
        if (st.ok() && undo_hooks_ != nullptr)
        {
          // 新插入的行被撤销 → 它的索引项也必须撤掉，否则索引里留下指向空洞的键
          undo_hooks_->OnUndoInsertDeleted(u.table, u.rid);
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
        if (st.ok() && undo_hooks_ != nullptr)
        {
          // 旧内容按**新**物理位置复插：索引项必须按新 Rid 重建（旧键已随新版本删除）
          undo_hooks_->OnUndoRowRestored(u.table, ignored, u.before);
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
      WriteJournal(os.str());
    }
    DbLogWarn(logcat::kTxn, os.str());
    return rb;
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
