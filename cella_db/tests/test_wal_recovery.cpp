// test_wal_recovery.cpp —— 崩溃恢复（阶段 P2）。
//
// 覆盖：
//   P2.1 日志格式：LSN 单调、七种记录类型齐全、编解码往返、坏尾被识别并截断
//   P2.2 Write-Ahead：提交即落盘、存盘点前先刷日志、存盘点后截断日志
//   P2.3 分析阶段：重启时重建事务表（ATT）与脏页表（DPT）
//   P2.4 Redo：已提交事务的改动在崩溃后仍然存在（INSERT / DELETE / UPDATE 三种）
//   P2.5 Undo：未提交事务的改动在崩溃后被撤销
//   P2.6 崩溃注入：写 → 强杀 → 重启 → 断言「已提交的在、未提交的没了」
//
// 崩溃怎么模拟：见 DbEngine::SimulateCrash()。它把**崩溃瞬间的磁盘状态**
// （数据文件 + WAL 文件，缓冲池里没刷出来的脏页全部丢弃）固定下来并覆盖工作目录，
// 因此重启面对的现场与真机被杀完全一致 —— 而且可重复、不需要子进程。
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cella/db/engine/db_engine.h"
#include "cella/db/wal/recovery_manager.h"
#include "cella/db/wal/wal_manager.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

const char* kSeed =
    "CREATE TABLE account(id INT PRIMARY KEY, balance INT);"
    "INSERT INTO account VALUES (1,100),(2,200),(3,300);";

// 模拟强杀并重启。返回 false 表示崩溃/重启本身就没成功。
bool CrashRestart(Engine* e) {
  if (!e->engine.SimulateCrash().ok()) {
    return false;
  }
  return e->Reopen();
}

// 取某表全表扫描结果的文本（id 升序）
std::string IdsOf(Engine* e, const char* table = "account") {
  const ScriptReport r = e->Run(std::string("get id in ") + table + " ordered id asc;");
  if (r.statements.empty()) {
    return std::string("<无语句>");
  }
  return RowsText(r.statements[0].result);
}

std::string BalancesOf(Engine* e) {
  const ScriptReport r = e->Run("get id, balance in account ordered id asc;");
  if (r.statements.empty()) {
    return std::string("<无语句>");
  }
  return RowsText(r.statements[0].result);
}

// 读日志里出现的记录类型计数
struct TypeCount {
  size_t begin = 0;
  size_t commit = 0;
  size_t abort = 0;
  size_t insert = 0;
  size_t del = 0;
  size_t update = 0;
  size_t checkpoint = 0;
};
TypeCount CountTypes(const std::vector<wal::WalRecord>& recs) {
  TypeCount c;
  for (const auto& r : recs) {
    switch (r.type) {
      case wal::RecordType::kBegin: ++c.begin; break;
      case wal::RecordType::kCommit: ++c.commit; break;
      case wal::RecordType::kAbort: ++c.abort; break;
      case wal::RecordType::kInsert: ++c.insert; break;
      case wal::RecordType::kDelete: ++c.del; break;
      case wal::RecordType::kUpdate: ++c.update; break;
      case wal::RecordType::kCheckpoint: ++c.checkpoint; break;
    }
  }
  return c;
}

}  // namespace

// ═════════════════════════════════════════════════════════════
// P2.1 日志格式
// ═════════════════════════════════════════════════════════════

MT_TEST(P21_日志含七类记录且LSN单调)
{
  Engine e("p21_types");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("BEGIN;"
                 "INSERT INTO account VALUES (4,400);"
                 "UPDATE account SET balance = 999 limit id = 4;"
                 "DELETE in account limit id = 4;"
                 "COMMIT;")
                .all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (5,500);ROLLBACK;").all_ok());

  wal::WalManager* w = e.engine.wal();
  MT_CHECK(w != nullptr);
  std::vector<wal::WalRecord> recs;
  MT_CHECK(w->ReadAll(&recs).ok());

  const TypeCount c = CountTypes(recs);
  MT_CHECK(c.begin >= 2u);
  MT_CHECK(c.commit >= 1u);
  MT_CHECK(c.abort >= 1u);
  MT_CHECK(c.insert >= 4u);   // 3 行种子 + 1 行
  MT_CHECK(c.update >= 1u);
  MT_CHECK(c.del >= 1u);
  MT_CHECK(c.checkpoint >= 1u);  // DDL 之后必定紧跟一次存盘点

  wal::lsn_t prev = 0;
  for (const auto& r : recs) {
    MT_CHECK(r.lsn > prev);  // 严格单调递增
    prev = r.lsn;
  }
}

MT_TEST(P21_行变更记录带前后像)
{
  Engine e("p21_images");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("UPDATE account SET balance = 111 limit id = 1;").all_ok());
  MT_CHECK(e.Run("DELETE in account limit id = 2;").all_ok());

  std::vector<wal::WalRecord> recs;
  MT_CHECK(e.engine.wal()->ReadAll(&recs).ok());

  bool saw_update = false;
  bool saw_delete = false;
  for (const auto& r : recs) {
    if (r.type == wal::RecordType::kUpdate && r.table == "account") {
      MT_EQ(r.before.size(), 2u);
      MT_EQ(r.after.size(), 2u);
      MT_EQ(r.before[0].int32_val, 1);    // 旧 id
      MT_EQ(r.after[1].int32_val, 111);   // 新 balance
      saw_update = true;
    }
    if (r.type == wal::RecordType::kDelete && r.table == "account") {
      MT_EQ(r.before.size(), 2u);
      MT_EQ(r.before[0].int32_val, 2);
      saw_delete = true;
    }
  }
  MT_CHECK(saw_update);
  MT_CHECK(saw_delete);
}

// 坏尾：崩溃时最后一条记录只写了一半。重开必须认出它并截断，
// 否则新记录会接在半截记录后面，整段日志从此再也读不出来。
MT_TEST(P21_崩溃留下的半截记录被截断)
{
  Engine e("p21_torn_tail");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());

  const std::string wal_path = e.engine.wal()->path();
  std::vector<wal::WalRecord> before;
  MT_CHECK(e.engine.wal()->ReadAll(&before).ok());
  MT_CHECK(before.size() >= 2u);

  // 手工往文件尾部追加 7 个垃圾字节（不足一条记录头，必然被判为坏尾）
  {
    std::ofstream f(wal_path, std::ios::binary | std::ios::app);
    f.write("\x01\x02\x03\x04\x05\x06\x07", 7);
    f.flush();
  }

  wal::WalManager reopened;
  wal::OpenResult opened;
  MT_CHECK(reopened.Open(wal_path, &opened).ok());
  MT_CHECK(opened.truncated_bytes == 7u);
  MT_EQ(opened.records, before.size());

  std::vector<wal::WalRecord> after;
  MT_CHECK(reopened.ReadAll(&after).ok());
  MT_EQ(after.size(), before.size());
  MT_EQ(after.back().lsn, before.back().lsn);
}

// ═════════════════════════════════════════════════════════════
// P2.2 Write-Ahead 规则
// ═════════════════════════════════════════════════════════════

MT_TEST(P22_提交后日志已落盘)
{
  Engine e("p22_force_at_commit");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());

  wal::WalManager* w = e.engine.wal();
  MT_CHECK(w != nullptr);
  const wal::lsn_t before = w->durable_lsn();
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (9,900);COMMIT;").all_ok());
  // 提交返回成功 = 该事务的日志已经在盘上（否则「提交成功却丢数据」）
  MT_CHECK(w->durable_lsn() > before);
  MT_CHECK(w->buffered_bytes() == 0u);
}

MT_TEST(P22_未提交事务的改动也进了日志)
{
  Engine e("p22_uncommitted_logged");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (7,700);").all_ok());

  std::vector<wal::WalRecord> recs;
  MT_CHECK(e.engine.wal()->ReadAll(&recs).ok());
  bool found = false;
  for (const auto& r : recs) {
    if (r.type == wal::RecordType::kInsert && r.after.size() == 2u &&
        r.after[0].int32_val == 7) {
      found = true;
    }
  }
  // 必须找得到：崩溃后要靠它把这个未提交事务撤掉
  MT_CHECK(found);
}

MT_TEST(P22_存盘点先刷日志再刷脏页并截断日志)
{
  Engine e("p22_checkpoint_truncates");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());

  wal::WalManager* w = e.engine.wal();
  std::vector<wal::WalRecord> before;
  MT_CHECK(w->ReadAll(&before).ok());
  MT_CHECK(before.size() >= 4u);

  MT_CHECK(e.engine.Checkpoint().ok());
  std::vector<wal::WalRecord> after;
  MT_CHECK(w->ReadAll(&after).ok());

  // 无活动事务 → 存盘点之后所有「数据记录」都可以丢（改动都随脏页落盘了）；
  // checkpoint 记录本身作为「新日志起点」保留（ARIES 的截断边界，供下次恢复定位）。
  size_t data_after = 0;
  for (const auto &r : after) {
    if (r.is_data_record()) {
      ++data_after;
    }
  }
  MT_EQ(data_after, 0u);
  MT_CHECK(after.size() <= 1u);  // 至多只剩一条 checkpoint 标记
  // 但 LSN 计数不回退继续单调（新记录不会与旧记录撞号）
  MT_CHECK(w->next_lsn() > before.back().lsn);
}

MT_TEST(P22_存盘点保留仍活动事务的记录)
{
  Engine e("p22_checkpoint_keeps_active");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  // 留一个未提交事务在跑
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (8,800);").all_ok());

  const wal::lsn_t active_first = e.engine.wal()->next_lsn();  // 该事务后续记录的起点
  (void)active_first;
  MT_CHECK(e.engine.Checkpoint().ok());

  wal::WalManager* w = e.engine.wal();
  std::vector<wal::WalRecord> recs;
  MT_CHECK(w->ReadAll(&recs).ok());
  // 至少要有 checkpoint 记录 + 那个未提交事务的 BEGIN / INSERT
  MT_CHECK(recs.size() >= 3u);
  bool has_ckpt = false;
  bool has_active_insert = false;
  for (const auto& r : recs) {
    if (r.type == wal::RecordType::kCheckpoint) {
      has_ckpt = true;
    }
    if (r.type == wal::RecordType::kInsert && r.after.size() == 2u &&
        r.after[0].int32_val == 8) {
      has_active_insert = true;
    }
  }
  MT_CHECK(has_ckpt);
  MT_CHECK(has_active_insert);
}

// ═════════════════════════════════════════════════════════════
// P2.3 / P2.4 / P2.5 —— 崩溃重启后的三阶段恢复
// ═════════════════════════════════════════════════════════════

MT_TEST(P24_崩溃后已提交的插入仍在)
{
  Engine e("p24_redo_insert");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (5,500);COMMIT;").all_ok());
  MT_EQ(IdsOf(&e), std::string("1\n2\n3\n4\n5"));

  MT_CHECK(CrashRestart(&e));
  // 数据文件里很可能一行都没有（脏页都在缓冲池里随进程一起没了），
  // 全靠 WAL 重做出来 —— 这正是 WAL 的核心承诺。
  MT_EQ(IdsOf(&e), std::string("1\n2\n3\n4\n5"));
  MT_EQ(BalancesOf(&e), std::string("1|100\n2|200\n3|300\n4|400\n5|500"));
}

MT_TEST(P24_崩溃后已提交的删除仍生效)
{
  Engine e("p24_redo_delete");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("DELETE in account limit id = 2;").all_ok());
  MT_EQ(IdsOf(&e), std::string("1\n3"));

  MT_CHECK(CrashRestart(&e));
  MT_EQ(IdsOf(&e), std::string("1\n3"));
}

MT_TEST(P24_崩溃后已提交的更新仍生效)
{
  Engine e("p24_redo_update");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("UPDATE account SET balance = 777 limit id = 2;").all_ok());

  MT_CHECK(CrashRestart(&e));
  MT_EQ(BalancesOf(&e), std::string("1|100\n2|777\n3|300"));
}

MT_TEST(P25_崩溃后未提交事务被撤销)
{
  Engine e("p25_undo");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (6,600);").all_ok());
  MT_CHECK(e.session().in_transaction());

  MT_CHECK(CrashRestart(&e));
  // 未提交的那行必须消失（崩溃 = 隐式回滚）
  MT_EQ(IdsOf(&e), std::string("1\n2\n3"));
}

MT_TEST(P25_崩溃后未提交的删除与更新被撤销)
{
  Engine e("p25_undo_delete_update");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("BEGIN;"
                 "DELETE in account limit id = 1;"
                 "UPDATE account SET balance = 555 limit id = 3;")
                .all_ok());
  MT_CHECK(e.session().in_transaction());

  MT_CHECK(CrashRestart(&e));
  MT_EQ(BalancesOf(&e), std::string("1|100\n2|200\n3|300"));
}

MT_TEST(P26_崩溃注入_已提交的在_未提交的没了)
{
  Engine e("p26_mixed");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  // 已提交
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (5,500);COMMIT;").all_ok());
  // 未提交
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (6,600);").all_ok());

  MT_CHECK(CrashRestart(&e));
  MT_EQ(IdsOf(&e), std::string("1\n2\n3\n4\n5"));
}

MT_TEST(P23_分析阶段重建事务表与脏页表)
{
  Engine e("p23_analyze");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (6,600);").all_ok());

  MT_CHECK(CrashRestart(&e));
  const wal::RecoveryStats& st = e.engine.recovery_stats();
  MT_CHECK(st.ran);                       // 确实跑了恢复
  MT_CHECK(st.records_scanned >= 4u);     // 扫描了日志记录
  MT_CHECK(st.txn_undone >= 1u);          // 撤销了那个未提交事务
  MT_CHECK(st.undo_records >= 1u);
  MT_CHECK(st.dpt_pages >= 1u);           // 脏页表非空
  MT_CHECK(st.redo_replayed >= 1u);       // 重做了已提交的改动
}

MT_TEST(P26_恢复后立刻存盘_日志被清空)
{
  Engine e("p26_recover_then_checkpoint");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());

  MT_CHECK(CrashRestart(&e));
  MT_EQ(IdsOf(&e), std::string("1\n2\n3\n4"));

  // 恢复完成后引擎会自动存一次盘并把日志整体清空 —— 于是二次重启无需再恢复
  // （已重放的数据都已落盘，日志里不再携带任何「数据记录」）。
  // 注意：上面这条 IdsOf 的 SELECT 自身跑在自动提交事务里，会写一条
  // kBegin + kCommit 的控制记录对，所以日志通常不是「绝对空」，而是「没有数据记录」。
  wal::WalManager* w = e.engine.wal();
  std::vector<wal::WalRecord> recs;
  MT_CHECK(w->ReadAll(&recs).ok());
  size_t data_recs = 0;
  for (const auto &r : recs) {
    if (r.is_data_record()) {
      ++data_recs;
    }
  }
  MT_EQ(data_recs, 0u);  // 已提交的数据改动都已落盘，日志里不再有可重放的脏数据
}

MT_TEST(P26_连续两次崩溃_结果仍然正确)
{
  Engine e("p26_twice");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());
  MT_CHECK(CrashRestart(&e));
  MT_EQ(IdsOf(&e), std::string("1\n2\n3\n4"));

  // 再来一次：恢复出来的状态必须能继续正常工作，再崩溃也扛得住
  MT_CHECK(e.Run("INSERT INTO account VALUES (5,500);").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (9,900);").all_ok());
  MT_CHECK(CrashRestart(&e));
  MT_EQ(IdsOf(&e), std::string("1\n2\n3\n4\n5"));
}

// 恢复必须幂等：再崩一次（期间不写任何东西）后重启，结果一模一样。
// 重放一条已经生效的记录什么都不该做 —— 否则每次恢复都会多出一行。
MT_TEST(P26_重做是幂等的)
{
  Engine e("p26_idempotent");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());
  MT_CHECK(CrashRestart(&e));
  const std::string first = IdsOf(&e);
  MT_EQ(first, std::string("1\n2\n3\n4"));
  const wal::lsn_t lsn_after_first = e.engine.wal()->next_lsn();

  MT_CHECK(CrashRestart(&e));
  MT_EQ(IdsOf(&e), first);
  // 第二次恢复不该让 LSN 乱跳（日志被正确截断，而不是无限增长）
  MT_CHECK(e.engine.wal()->next_lsn() < lsn_after_first + 16u);
}

MT_TEST(P26_恢复后索引与表数据一致)
{
  Engine e("p26_index_consistent");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  MT_CHECK(e.Run("CREATE INDEX idx_balance ON account (balance);").all_ok());
  MT_CHECK(e.Run("INSERT INTO account VALUES (4,400);").all_ok());
  MT_CHECK(e.Run("UPDATE account SET balance = 250 limit id = 2;").all_ok());
  MT_CHECK(e.Run("BEGIN;INSERT INTO account VALUES (6,600);").all_ok());

  MT_CHECK(CrashRestart(&e));
  MT_EQ(BalancesOf(&e), std::string("1|100\n2|250\n3|300\n4|400"));
  // 索引是派生数据：恢复后用它查，结果必须与全表扫描一致
  const ScriptReport via_index = e.Run("get id in account limit balance = 250;");
  MT_CHECK(via_index.all_ok());
  MT_EQ(RowsText(via_index.statements[0].result), std::string("2"));
}

MT_TEST(P26_干净关闭不需要恢复)
{
  Engine e("p26_clean_close");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run(kSeed).all_ok());
  e.Close();
  MT_CHECK(e.Reopen());
  // 干净关闭时数据已全部落盘、日志已清空 → 不该触发恢复
  MT_CHECK(!e.engine.recovery_stats().ran);
  MT_EQ(IdsOf(&e), std::string("1\n2\n3"));
}

// 关掉 WAL 时行为必须回退到「改造前」：提交只留在缓冲池，强杀就丢。
// 这条用例是刻意的对照组 —— 它说明上面的用例不是靠运气通过的。
MT_TEST(P26_关闭WAL后崩溃即丢数据)
{
  const std::string dir = testutil::FreshDir("p26_no_wal");
  {
    DbEngine eng;
    EngineConfig c;
    c.data_dir = dir;
    c.enable_log = false;
    c.enable_journal = false;  // 关掉 WAL
    MT_CHECK(eng.Open(c).ok());
    ScriptReport r;
    MT_CHECK(eng.default_session().Execute(kSeed, &r).ok());
    MT_CHECK(r.all_ok());
    MT_CHECK(eng.SimulateCrash().ok());
  }
  {
    DbEngine eng;
    EngineConfig c;
    c.data_dir = dir;
    c.enable_log = false;
    c.enable_journal = false;
    MT_CHECK(eng.Open(c).ok());
    ScriptReport r;
    (void)eng.default_session().Execute("get id in account ordered id asc;", &r);
    // 没有 WAL：种子数据大概率随缓冲池一起丢了（表也可能根本打不开）
    if (r.all_ok() && !r.statements.empty()) {
      MT_CHECK(RowsText(r.statements[0].result) != std::string("1\n2\n3"));
    }
  }
}
