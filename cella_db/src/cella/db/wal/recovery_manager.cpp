// recovery_manager.cpp —— 分析 / 重做 / 撤销三阶段的实现。
#include "cella/db/wal/recovery_manager.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "cella/db/common/db_logger.h"

namespace cella::db::wal {

std::string RecoveryStats::ToText() const {
  std::ostringstream os;
  if (!ran) {
    return "本次启动未执行崩溃恢复（日志为空或干净关闭）";
  }
  os << "崩溃恢复完成：扫描 " << records_scanned << " 条记录（LSN 1.." << end_lsn << "）\n";
  os << "  最后一个存盘点 LSN=" << (checkpoint_lsn == kInvalidLsn ? 0 : checkpoint_lsn)
     << "，redo 起点 LSN=" << (redo_start_lsn == kInvalidLsn ? 0 : redo_start_lsn) << "\n";
  os << "  事务表：共 " << txn_total << "（已提交 " << txn_committed << " / 已回滚 "
     << txn_aborted << " / 本次撤销 " << txn_undone << "）\n";
  os << "  脏页表：" << dpt_pages << " 页\n";
  os << "  重放 " << redo_replayed << " 条（跳过 " << redo_skipped << " 条），撤销 "
     << undo_records << " 条";
  return os.str();
}

RecoveryManager::RecoveryManager(WalManager* wal, TxnManager* txns, Executor* exec)
    : wal_(wal), txns_(txns), exec_(exec) {}

bool RecoveryManager::NeedRedo(const TxnEntry& e) const {
  if (e.data_records.empty()) {
    return false;
  }
  if (e.finished()) {
    // 结束时点早于最后一个存盘点 → 改动已随存盘点落盘；否则必须重放
    return e.end_lsn > checkpoint_lsn_;
  }
  return true;  // 仍活动：先重放到崩溃瞬间，再撤销
}

DbStatus RecoveryManager::Recover(RecoveryStats* out) {
  const auto t0 = std::chrono::steady_clock::now();
  stats_ = RecoveryStats{};
  records_.clear();
  att_.clear();
  dpt_.clear();
  checkpoint_lsn_ = kInvalidLsn;
  redo_start_ = kInvalidLsn;

  const DbStatus rs = wal_->ReadAll(&records_);
  if (!rs.ok()) {
    return rs;
  }
  if (records_.empty()) {
    // 空日志：要么是新库，要么是上次干净关闭时清空的 —— 不需要恢复
    if (out != nullptr) {
      *out = stats_;
    }
    return DbStatus::Ok();
  }
  stats_.ran = true;
  stats_.records_scanned = records_.size();
  stats_.end_lsn = records_.back().lsn;

  DbStatus s = Analyze();
  if (!s.ok()) {
    return s;
  }
  s = Redo();
  if (!s.ok()) {
    return s;
  }
  s = Undo();
  if (!s.ok()) {
    return s;
  }
  // 重放/撤销都按内容定位行，索引是跟着一起维护的；但索引页可能停在半个状态，
  // 所以最后按最终表数据整体重建一次 —— 这是「索引与表一致」的兜底保证。
  s = exec_->RebuildIndexes(touched_tables_);
  if (!s.ok()) {
    return s;
  }

  const auto t1 = std::chrono::steady_clock::now();
  stats_.elapsed_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  stats_.redo_start_lsn = redo_start_;
  stats_.checkpoint_lsn = checkpoint_lsn_;
  stats_.dpt_pages = dpt_.size();
  if (out != nullptr) {
    *out = stats_;
  }
  return DbStatus::Ok();
}

// ── ① 分析阶段 ──────────────────────────────────────────────
DbStatus RecoveryManager::Analyze() {
  for (size_t i = 0; i < records_.size(); ++i) {
    const WalRecord& r = records_[i];
    TxnEntry* e = nullptr;
    if (r.type == RecordType::kCheckpoint) {
      checkpoint_lsn_ = r.lsn;
      continue;
    }
    {
      auto it = att_.find(r.txn_id);
      if (it == att_.end()) {
        it = att_.emplace(r.txn_id, TxnEntry{}).first;
      }
      e = &it->second;
    }
    e->last_lsn = r.lsn;
    switch (r.type) {
      case RecordType::kBegin:
        if (e->first_lsn == kInvalidLsn) {
          e->first_lsn = r.lsn;
        }
        break;
      case RecordType::kCommit:
        e->committed = true;
        e->end_lsn = r.lsn;
        break;
      case RecordType::kAbort:
        e->aborted = true;
        e->end_lsn = r.lsn;
        break;
      case RecordType::kInsert:
      case RecordType::kDelete:
      case RecordType::kUpdate:
        if (e->first_lsn == kInvalidLsn) {
          e->first_lsn = r.lsn;
        }
        e->data_records.push_back(i);
        break;
      case RecordType::kCheckpoint:
        break;
    }
  }

  // 脏页表：自最后一个存盘点以来被改动过的（表, 页）。
  // 存盘点之前改过的页已经随存盘点落盘，不再算「脏」。
  for (const auto& r : records_) {
    if (!r.is_data_record()) {
      continue;
    }
    if (checkpoint_lsn_ != kInvalidLsn && r.lsn <= checkpoint_lsn_) {
      continue;
    }
    dpt_.insert({r.table, r.page_id});
  }

  // 受影响的表：索引是派生数据，重放前清空、重放后重建都按这个集合来
  for (const auto& r : records_) {
    if (r.is_data_record() && !r.table.empty()) {
      touched_tables_.insert(r.table);
    }
  }

  // redo 起点 = 所有「需要重放」的事务里最早的首条记录 LSN
  lsn_t start = kInvalidLsn;
  for (const auto& kv : att_) {
    const TxnEntry& e = kv.second;
    if (!NeedRedo(e)) {
      continue;
    }
    if (start == kInvalidLsn || e.first_lsn < start) {
      start = e.first_lsn;
    }
  }
  // 全都无需重放 → 起点设在日志末尾之后（等价于一次都不重放）
  redo_start_ = (start == kInvalidLsn) ? stats_.end_lsn + 1 : start;
  return DbStatus::Ok();
}

// ── ② 重做阶段：重复历史（连未提交事务也重放，撤销留到 ③）──
DbStatus RecoveryManager::Redo() {
  // 索引不进日志，而索引页同样可能被淘汰到磁盘 → 崩溃后它停在半个状态。
  // 先清空再重放，残键就不会在重放时造成「幽灵唯一冲突」。
  const DbStatus cs = exec_->ClearIndexes(touched_tables_);
  if (!cs.ok()) {
    return cs;
  }
  for (const auto& r : records_) {
    if (!r.is_data_record()) {
      continue;
    }
    if (r.lsn < redo_start_) {
      ++stats_.redo_skipped;
      continue;
    }
    DbStatus s;
    switch (r.type) {
      case RecordType::kInsert:
        s = exec_->RecoveryInsertRow(r.table, r.after);
        break;
      case RecordType::kDelete:
        s = exec_->RecoveryDeleteRow(r.table, r.before);
        break;
      case RecordType::kUpdate:
        s = exec_->RecoveryReplaceRow(r.table, r.before, r.after);
        break;
      default:
        continue;
    }
    if (!s.ok()) {
      // 重放失败不能再继续：后面的记录建立在前面的状态之上
      return DbStatus::Error(DbCode::kWalError, "重做 LSN=" + std::to_string(r.lsn) +
                                                    " 失败: " + s.ToString());
    }
    ++stats_.redo_replayed;
  }
  return DbStatus::Ok();
}

// ── ③ 撤销阶段：回滚失败者事务 ──────────────────────────────
DbStatus RecoveryManager::Undo() {
  stats_.txn_total = att_.size();
  for (const auto& kv : att_) {
    if (kv.second.committed) {
      ++stats_.txn_committed;
    } else if (kv.second.aborted) {
      ++stats_.txn_aborted;
    }
  }

  DbStatus first_error;
  for (const auto& kv : att_) {
    const txn_id_t id = kv.first;
    const TxnEntry& e = kv.second;
    if (e.finished()) {
      continue;  // 已提交 / 已回滚 —— 不是失败者
    }
    if (e.data_records.empty()) {
      continue;
    }
    // 按日志顺序取出该事务的行变更，交给 TxnManager 逆序撤销
    std::vector<WalRecord> mine;
    mine.reserve(e.data_records.size());
    for (size_t idx : e.data_records) {
      mine.push_back(records_[idx]);
    }
    const DbStatus s = txns_->UndoWalRecords(id, mine, exec_, "崩溃时未提交");
    stats_.undo_records += mine.size();
    if (s.ok()) {
      ++stats_.txn_undone;
    } else if (first_error.ok()) {
      first_error = s;  // 记第一个失败，但继续撤销其余事务（能救多少救多少）
    }
  }
  return first_error;
}

}  // namespace cella::db::wal
