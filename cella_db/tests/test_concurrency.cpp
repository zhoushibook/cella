// test_concurrency.cpp —— 并发控制测试。
//   ① 锁管理器单元级：相容矩阵、阻塞/唤醒、超时兜底、死锁检测与牺牲者选择。
//   ② 会话级：读读并发、写锁互斥（串行化）、跨会话死锁检测与牺牲者回滚。
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cella/db/txn/lock_manager.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

constexpr auto kShort = std::chrono::milliseconds(150);

void SleepFor(std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }

}  // namespace

MT_TEST(锁_共享相容与排他互斥) {
  LockManager lm(kShort);
  MT_CHECK(lm.Acquire(1, "t", LockMode::kShared).ok());
  MT_CHECK(lm.Acquire(2, "t", LockMode::kShared).ok());  // S 与 S 相容
  MT_CHECK(lm.HoldsAny(1));
  MT_CHECK(lm.HoldsAny(2));

  // 第三个事务请求 X：与共享持有者冲突 → 超时（DB-605）
  const DbStatus s = lm.Acquire(3, "t", LockMode::kExclusive);
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kLockConflict);
  MT_EQ(lm.WaiterCount(), 0u);  // 退出等待后不留残影

  lm.ReleaseAll(1);
  lm.ReleaseAll(2);
  MT_CHECK(!lm.HoldsAny(1));
  MT_CHECK(lm.Acquire(3, "t", LockMode::kExclusive).ok());
  lm.ReleaseAll(3);
  MT_CHECK(!lm.HoldsAny(3));
}

MT_TEST(锁_不同资源互不影响) {
  LockManager lm(kShort);
  MT_CHECK(lm.Acquire(1, "a", LockMode::kExclusive).ok());
  MT_CHECK(lm.Acquire(2, "b", LockMode::kExclusive).ok());
  // 1 请求 b（已被 2 持 X）→ 冲突；但 1 在 a 上的锁不受影响
  MT_CHECK(lm.Acquire(1, "b", LockMode::kShared).code() == DbCode::kLockConflict);
  MT_CHECK(lm.HoldsAny(1));
  MT_CHECK(lm.HoldsAny(2));
  // 重复申请自己已持有的锁 → 相容，立即授予
  MT_CHECK(lm.Acquire(1, "a", LockMode::kExclusive).ok());
  lm.ReleaseAll(1);
  lm.ReleaseAll(2);
  MT_CHECK(!lm.HoldsAny(1));
  MT_CHECK(!lm.HoldsAny(2));
}

MT_TEST(锁_排他阻塞共享并唤醒) {
  LockManager lm(std::chrono::seconds(5));
  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());

  std::atomic<bool> acquired{false};
  std::thread waiter([&] {
    if (lm.Acquire(2, "t", LockMode::kShared).ok()) {
      acquired = true;
    }
  });
  SleepFor(std::chrono::milliseconds(100));
  MT_CHECK(!acquired.load());           // 被 X 挡住
  MT_EQ(lm.WaiterCount(), 1u);

  lm.ReleaseAll(1);                      // 释放 → 唤醒
  waiter.join();
  MT_CHECK(acquired.load());
  lm.ReleaseAll(2);
}

MT_TEST(锁_升级需等待其它共享者) {
  LockManager lm(kShort);
  MT_CHECK(lm.Acquire(1, "t", LockMode::kShared).ok());
  MT_CHECK(lm.Acquire(2, "t", LockMode::kShared).ok());
  // 1 想升级为 X：2 仍持 S → 冲突（超时）
  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).code() == DbCode::kLockConflict);
  lm.ReleaseAll(2);
  // 2 退出后 1 可以直接升级（自己持有的 S 不算冲突）
  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());
  lm.ReleaseAll(1);
}

MT_TEST(锁_死锁检测与牺牲者) {
  LockManager lm(std::chrono::seconds(5));
  MT_CHECK(lm.Acquire(1, "A", LockMode::kExclusive).ok());
  MT_CHECK(lm.Acquire(2, "B", LockMode::kExclusive).ok());

  // 1 请求 B：2 没在等任何锁 → 只是阻塞，不是死锁
  std::atomic<int> rc1{-1};
  std::thread t1([&] {
    const DbStatus s = lm.Acquire(1, "B", LockMode::kExclusive);
    rc1 = s.ok() ? 0 : static_cast<int>(s.code());
  });
  SleepFor(std::chrono::milliseconds(100));
  MT_EQ(rc1.load(), -1);                 // 仍在等待
  MT_EQ(lm.DeadlockCount(), 0u);

  // 2 请求 A：与 1 的等待形成环 → 请求者 2 被选为牺牲者
  const DbStatus s2 = lm.Acquire(2, "A", LockMode::kExclusive);
  MT_CHECK(!s2.ok());
  MT_CHECK(s2.code() == DbCode::kDeadlock);
  MT_EQ(lm.DeadlockCount(), 1u);
  MT_CHECK(lm.DumpWaitForGraph().find("->") != std::string::npos);

  // 牺牲者放弃后释放，1 应拿到 B
  lm.ReleaseAll(2);
  t1.join();
  MT_EQ(rc1.load(), 0);
  MT_CHECK(lm.HoldsAny(1));
  lm.ReleaseAll(1);
}

MT_TEST(并发_读读不阻塞) {
  Engine e("conc_rr");
  MT_CHECK(e.Run("CREATE TABLE t(id INT);INSERT INTO t VALUES (1),(2);").all_ok());

  Session s1(&e.engine, "rr1");
  Session s2(&e.engine, "rr2");

  std::atomic<int> ok2{0};
  std::thread t2([&] {
    ScriptReport r;
    if (s2.Execute("get id in t;", &r).ok()) {
      ok2 = 1;
    }
  });
  ScriptReport r1;
  MT_CHECK(s1.Execute("get id in t;", &r1).ok());
  t2.join();
  MT_EQ(ok2.load(), 1);
}

MT_TEST(并发_写锁互斥直到提交) {
  Engine e("conc_write");
  MT_CHECK(e.Run("CREATE TABLE t(id INT, v INT);INSERT INTO t VALUES (1,10);").all_ok());

  Session s1(&e.engine, "w1");
  Session s2(&e.engine, "w2");

  ScriptReport r1;
  MT_CHECK(s1.Execute("BEGIN;UPDATE t SET v = 11 limit id = 1;", &r1).ok());
  MT_CHECK(e.engine.locks().HoldsAny(s1.current_txn()));

  std::atomic<bool> done{false};
  std::atomic<int> code{-1};
  std::thread t2([&] {
    ScriptReport r2;
    (void)s2.Execute("UPDATE t SET v = 22 limit id = 1;", &r2);
    code = static_cast<int>(r2.statements[0].status.code());
    done = true;
  });
  SleepFor(std::chrono::milliseconds(150));
  MT_CHECK(!done.load());  // 被 s1 的 X 锁挡住
  MT_EQ(e.engine.locks().WaiterCount(), 1u);

  MT_CHECK(s1.Execute("COMMIT;", &r1).ok());
  t2.join();
  MT_CHECK(done.load());
  MT_EQ(code.load(), static_cast<int>(DbCode::kOk));

  // 串行化：s1 先写 11，s2 后写 22
  MT_EQ(RowsText(e.Run("get v in t;").statements[0].result), std::string("22"));
}

MT_TEST(并发_跨会话死锁与牺牲者回滚) {
  Engine e("conc_deadlock");
  MT_CHECK(e.Run("CREATE TABLE a(id INT, v INT);CREATE TABLE b(id INT, v INT);"
                 "INSERT INTO a VALUES (1,1);INSERT INTO b VALUES (1,1);")
               .all_ok());

  Session s1(&e.engine, "dead1");
  Session s2(&e.engine, "dead2");
  ScriptReport ra;
  MT_CHECK(s1.Execute("BEGIN;UPDATE a SET v = 2 limit id = 1;", &ra).ok());
  MT_CHECK(s2.Execute("BEGIN;UPDATE b SET v = 2 limit id = 1;", &ra).ok());

  std::atomic<bool> d1_done{false};
  std::atomic<int> d1_code{-1};
  std::thread t1([&] {
    ScriptReport r;
    (void)s1.Execute("UPDATE b SET v = 3 limit id = 1;", &r);
    d1_code = static_cast<int>(r.statements[0].status.code());
    d1_done = true;
  });
  SleepFor(std::chrono::milliseconds(150));
  MT_CHECK(!d1_done.load());  // dead1 卡在等待

  // dead2 反向请求 → 成环 → dead2 成为牺牲者
  ScriptReport rb;
  (void)s2.Execute("UPDATE a SET v = 3 limit id = 1;", &rb);
  MT_CHECK(rb.statements[0].status.code() == DbCode::kDeadlock);
  MT_CHECK(!s2.in_transaction());  // 牺牲者被整事务回滚
  MT_CHECK(e.engine.locks().DeadlockCount() >= 1u);

  t1.join();
  MT_CHECK(d1_done.load());
  MT_EQ(d1_code.load(), static_cast<int>(DbCode::kOk));
  MT_CHECK(s1.in_transaction());
  MT_CHECK(s1.Execute("COMMIT;", &ra).ok());

  // dead2 对 b 的修改已回滚：b.v 最终为 3（由 dead1 写入）
  MT_EQ(RowsText(e.Run("get v in b;").statements[0].result), std::string("3"));
  MT_EQ(RowsText(e.Run("get v in a;").statements[0].result), std::string("2"));
}

MT_TEST(并发_多线程混合读写无死锁无数据损坏) {
  Engine e("conc_mixed");
  MT_CHECK(e.Run("CREATE TABLE t(id INT NOT NULL, v INT NOT NULL);"
                 "INSERT INTO t VALUES (1,0),(2,0),(3,0),(4,0);")
               .all_ok());

  const int kThreads = 4;
  const int kIters = 12;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      Session s(&e.engine, "mixed" + std::to_string(i));
      for (int k = 0; k < kIters; ++k) {
        ScriptReport r;
        const int target = (i % 4) + 1;
        // 读 + 写交替；写锁互斥 + 语句级回滚保证不会互相破坏
        (void)s.Execute("get id in t limit id = " + std::to_string(target) + ";", &r);
        if (!r.all_ok()) {
          ++failures;
        }
        ScriptReport r2;
        (void)s.Execute("UPDATE t SET v = " + std::to_string(i * 100 + k) + " limit id = " +
                            std::to_string(target) + ";",
                        &r2);
        if (!r2.all_ok()) {
          ++failures;
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  MT_EQ(failures.load(), 0);

  // 四行都还在，且 v 都是合法写入过的值
  const ScriptReport r = e.Run("get id, v in t ordered id asc;");
  MT_CHECK(r.all_ok());
  MT_EQ(r.statements[0].result.rows.size(), 4u);
  MT_CHECK(e.Run("get * in t;").all_ok());
}
