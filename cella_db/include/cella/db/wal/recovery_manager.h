// recovery_manager.h —— 崩溃后的三阶段恢复：分析 → 重做 → 撤销。
//
// 教科书（ARIES）的三段在这里的对应关系：
//
//   ① 分析（Analyze）
//      顺序扫一遍日志，重建**事务表（ATT）**：每个事务的首条记录 LSN、末条记录
//      LSN、是否已提交/已回滚；同时重建**脏页表（DPT）**：自上一个存盘点以来
//      被改过的（表, 页）集合。再据此算出 redo 起点。
//
//   ② 重做（Redo）
//      从 redo 起点开始，按 LSN 顺序把**所有**事务（含后来被撤销的）的行变更
//      重新放一遍 —— 这就是「重复历史」。之所以连未提交事务也重放，是因为
//      缓冲池会随时淘汰脏页，磁盘上到底有没有那次改动是不确定的；
//      先把状态还原到崩溃瞬间，再统一撤销，逻辑最简单也最不容易错。
//
//   ③ 撤销（Undo）
//      对分析阶段认定「既没有提交记录也没有回滚记录」的事务（失败者/loser），
//      按 LSN **逆序**撤销：后做的先撤。撤销 = 反向重放（见 IUndoApplier）。
//
// 与 ARIES 的两处刻意差异（都是因为存储层不暴露 pageLSN）：
//   * 没有 pageLSN，无法用「页上 LSN ≥ 记录 LSN 就跳过」来剪枝；
//     取而代之的是**让重做本身幂等**（按主键/行内容定位，已生效就跳过）。
//   * 脏页表因此只用于诊断与对外解释「恢复范围」，不参与剪枝。
//   这两点都写在 docs/WAL_RECOVERY.md 的「已知边界」里。
#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cella/db/common/db_status.h"
#include "cella/db/exec/executor.h"
#include "cella/db/txn/transaction.h"
#include "cella/db/wal/wal_manager.h"
#include "cella/db/wal/wal_types.h"
#include "cella/storage/common/types.h"

namespace cella::db::wal {

// ── 一次恢复的统计（诊断输出 + 测试断言）────────────────────
struct RecoveryStats {
  size_t records_scanned = 0;   // 扫到的日志记录总数
  size_t redo_replayed = 0;     // 实际重放的行变更条数
  size_t redo_skipped = 0;      // 位于 redo 起点之前、无需重放的条数
  size_t txn_total = 0;         // 日志里出现过的事务数
  size_t txn_committed = 0;
  size_t txn_aborted = 0;
  size_t txn_undone = 0;        // 本次恢复中被撤销的失败事务数
  size_t undo_records = 0;      // 被撤销的行变更条数
  size_t dpt_pages = 0;         // 脏页表里的页数
  lsn_t checkpoint_lsn = kInvalidLsn;
  lsn_t redo_start_lsn = kInvalidLsn;
  lsn_t end_lsn = kInvalidLsn;  // 日志里最后一条记录的 LSN
  double elapsed_ms = 0.0;
  bool ran = false;             // 本次打开是否真的跑了恢复

  std::string ToText() const;
};

class RecoveryManager {
 public:
  // 分析阶段重建出来的一条事务表项
  struct TxnEntry {
    lsn_t first_lsn = kInvalidLsn;
    lsn_t last_lsn = kInvalidLsn;
    lsn_t end_lsn = kInvalidLsn;   // commit / abort 记录的 LSN
    bool committed = false;
    bool aborted = false;
    std::vector<size_t> data_records;  // 下标进 records_
    bool finished() const { return committed || aborted; }
  };

  RecoveryManager(WalManager* wal, TxnManager* txns, Executor* exec);

  DbStatus Recover(RecoveryStats* stats);

  // ── 分析阶段的产物（测试可直接断言）──
  const std::vector<WalRecord>& records() const { return records_; }
  const std::map<txn_id_t, TxnEntry>& att() const { return att_; }
  const std::set<std::pair<std::string, storage::page_id_t>>& dpt() const { return dpt_; }
  const RecoveryStats& stats() const { return stats_; }

 private:
  DbStatus Analyze();
  DbStatus Redo();
  DbStatus Undo();
  // 该事务是否需要在本次恢复中重放：
  //   * 仍活动（无 commit/abort）→ 必须重放，之后还要撤销；
  //   * 已结束但结束点在最后一个存盘点之后 → 存盘点没带上它，必须重放；
  //   * 已结束且结束点早于最后一个存盘点 → 改动已随存盘点落盘，跳过。
  bool NeedRedo(const TxnEntry& e) const;

  WalManager* wal_;
  TxnManager* txns_;
  Executor* exec_;
  // 日志里出现过的表：索引是派生数据，恢复前后要按它清空 + 重建
  std::set<std::string> touched_tables_;
  std::vector<WalRecord> records_;
  std::map<txn_id_t, TxnEntry> att_;
  std::set<std::pair<std::string, storage::page_id_t>> dpt_;
  lsn_t checkpoint_lsn_ = kInvalidLsn;
  lsn_t redo_start_ = kInvalidLsn;
  RecoveryStats stats_;
};

}  // namespace cella::db::wal
