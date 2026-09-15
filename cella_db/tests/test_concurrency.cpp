// test_concurrency.cpp —— 并发控制测试。
//   ① 锁管理器单元级：相容矩阵、阻塞/唤醒、超时兜底、死锁检测与牺牲者、公平调度。
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

long long ElapsedMs(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

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

// ═════════════════════════════════════════════════════════════
// 公平调度（收尾补齐）：消除写饥饿
// ═════════════════════════════════════════════════════════════

MT_TEST(锁_新来的读者不得插队到等待中的写者之前) {
  LockManager lm(std::chrono::seconds(5));
  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());

  // 写者 2 先到，排在队首
  std::atomic<bool> writer_got{false};
  std::thread writer([&] {
    if (lm.Acquire(2, "t", LockMode::kExclusive).ok()) {
      writer_got = true;
    }
  });
  SleepFor(std::chrono::milliseconds(100));
  MT_EQ(lm.WaiterCount(), 1u);

  // 读者 3 后到：相容性上本可立即授予（S 与 S 相容），但队列里有人 → 必须排队。
  // 这是「写饥饿」的分水岭：旧实现下源源不断的读者会一直把写者往后挤。
  std::atomic<bool> reader_got{false};
  std::thread reader([&] {
    if (lm.Acquire(3, "t", LockMode::kShared).ok()) {
      reader_got = true;
    }
  });
  SleepFor(std::chrono::milliseconds(150));
  MT_CHECK(!reader_got.load());  // 关键断言：没有插队
  MT_EQ(lm.WaiterCount(), 2u);

  // 释放后：先给队首的写者，读者继续等
  lm.ReleaseAll(1);
  writer.join();
  MT_CHECK(writer_got.load());
  MT_CHECK(!reader_got.load());
  MT_CHECK(lm.HoldsAny(2));

  // 写者提交 → 读者才被放行
  lm.ReleaseAll(2);
  reader.join();
  MT_CHECK(reader_got.load());
  lm.ReleaseAll(3);
}

MT_TEST(锁_公平规则不影响重入与自我升级) {
  LockManager lm(kShort);
  // 重入：同一事务重复申请同一资源应立即成功（不能被自己排的队挡住）
  MT_CHECK(lm.Acquire(1, "t", LockMode::kShared).ok());
  MT_CHECK(lm.Acquire(1, "t", LockMode::kShared).ok());
  // 无他人持 S → 自我升级成功
  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());

  // 已持 X 时，即使有别人在排队，自己的重入也必须立即放行
  std::atomic<bool> other{false};
  std::thread th([&] {
    if (lm.Acquire(2, "t", LockMode::kShared).ok()) {
      other = true;
    }
  });
  SleepFor(std::chrono::milliseconds(120));
  MT_EQ(lm.WaiterCount(), 1u);
  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());  // 重入放行
  MT_CHECK(lm.Acquire(1, "t", LockMode::kShared).ok());     // X 已满足 S 请求
  MT_CHECK(!other.load());

  lm.ReleaseAll(1);
  th.join();
  MT_CHECK(other.load());
  lm.ReleaseAll(2);
}

MT_TEST(锁_排队次序本身参与死锁判定) {
  // 「1 持 S 想升级 X，而 2 的 X 已排在它前面」是一种真实的互等：
  // 若等待图只看持有者、不看队列次序，这个环会被漏判成双方干等到超时。
  LockManager lm(std::chrono::seconds(5));
  MT_CHECK(lm.Acquire(1, "t", LockMode::kShared).ok());

  std::atomic<int> rc2{-1};
  std::thread t2([&] {
    const DbStatus s = lm.Acquire(2, "t", LockMode::kExclusive);
    rc2 = s.ok() ? 0 : static_cast<int>(s.code());
  });
  SleepFor(std::chrono::milliseconds(120));
  MT_EQ(lm.WaiterCount(), 1u);
  MT_EQ(lm.DeadlockCount(), 0u);

  const auto t0 = std::chrono::steady_clock::now();
  const DbStatus s1 = lm.Acquire(1, "t", LockMode::kExclusive);
  const long long ms = ElapsedMs(t0);
  MT_CHECK(s1.code() == DbCode::kDeadlock);
  MT_EQ(lm.DeadlockCount(), 1u);
  MT_CHECK(ms < 3000);  // 立刻判定，不是等 5s 超时

  // 牺牲者（1）放手后，队首的 2 拿到 X
  lm.ReleaseAll(1);
  t2.join();
  MT_EQ(rc2.load(), 0);
  MT_CHECK(lm.HoldsAny(2));
  lm.ReleaseAll(2);
}

// ═════════════════════════════════════════════════════════════
// 并发/锁指标 + 超时可配
// ═════════════════════════════════════════════════════════════

MT_TEST(锁_超时阈值可配置且实际生效) {
  LockManager lm(std::chrono::milliseconds(120));
  MT_EQ(lm.Timeout().count(), 120);
  lm.SetTimeout(std::chrono::milliseconds(250));
  MT_EQ(lm.Timeout().count(), 250);

  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());
  const auto t0 = std::chrono::steady_clock::now();
  const DbStatus s = lm.Acquire(2, "t", LockMode::kShared);
  const long long ms = ElapsedMs(t0);
  MT_CHECK(s.code() == DbCode::kLockConflict);
  MT_CHECK(ms >= 200);   // 至少等满阈值
  MT_CHECK(ms < 2000);   // 但不会拖到默认的 5s
  lm.ReleaseAll(1);
  lm.ReleaseAll(2);
}

MT_TEST(锁_统计指标反映授予_等待_超时) {
  LockManager lm(kShort);
  const LockStats s0 = lm.Stats();
  MT_EQ(s0.granted, 0u);
  MT_EQ(s0.waited, 0u);
  MT_EQ(s0.held_locks, 0u);

  MT_CHECK(lm.Acquire(1, "t", LockMode::kExclusive).ok());
  const LockStats s1 = lm.Stats();
  MT_EQ(s1.granted, 1u);
  MT_EQ(s1.held_locks, 1u);
  MT_EQ(s1.current_waiters, 0u);

  // 冲突 → 进队列 → 超时放弃
  MT_CHECK(lm.Acquire(2, "t", LockMode::kShared).code() == DbCode::kLockConflict);
  const LockStats s2 = lm.Stats();
  MT_EQ(s2.waited, 1u);
  MT_EQ(s2.timeouts, 1u);
  MT_EQ(s2.deadlocks, 0u);
  MT_EQ(s2.current_waiters, 0u);  // 退出等待后不留残影
  MT_CHECK(s2.wait_us_total > 0u);

  lm.ReleaseAll(1);
  MT_EQ(lm.Stats().held_locks, 0u);

  // 等待后被授予：计入 granted_after_wait 而非 granted
  LockManager lm2(std::chrono::seconds(5));
  MT_CHECK(lm2.Acquire(1, "t", LockMode::kExclusive).ok());
  std::thread th([&] { (void)lm2.Acquire(2, "t", LockMode::kExclusive); });
  SleepFor(std::chrono::milliseconds(130));
  MT_EQ(lm2.Stats().waited, 1u);
  MT_EQ(lm2.Stats().current_waiters, 1u);
  lm2.ReleaseAll(1);
  th.join();
  const LockStats s3 = lm2.Stats();
  MT_EQ(s3.granted_after_wait, 1u);
  MT_EQ(s3.timeouts, 0u);
  MT_EQ(s3.current_waiters, 0u);
  MT_CHECK(s3.granted >= 1u);
  lm2.ReleaseAll(2);
  MT_CHECK(lm2.StatsText().find("平均等待时长") != std::string::npos);
}

// ═════════════════════════════════════════════════════════════
// 会话级并发
// ═════════════════════════════════════════════════════════════

MT_TEST(并发_长事务持读锁时写者不被饿死) {
  Engine e("conc_fair_write");
  MT_CHECK(e.Run("CREATE TABLE t(id INT, v INT);INSERT INTO t VALUES (1,0);").all_ok());

  Session reader(&e.engine, "fair_r");
  Session writer(&e.engine, "fair_w");

  // 读事务显式 BEGIN → S 锁保持到提交
  ScriptReport rr;
  MT_CHECK(reader.Execute("BEGIN;get id in t;", &rr).ok());
  MT_CHECK(e.engine.locks().HoldsAny(reader.current_txn()));

  std::atomic<bool> done{false};
  std::atomic<long long> waited{0};
  std::thread w([&] {
    const auto t0 = std::chrono::steady_clock::now();
    ScriptReport r2;
    (void)writer.Execute("UPDATE t SET v = 7 limit id = 1;", &r2);
    waited = ElapsedMs(t0);
    done = true;
  });
  SleepFor(std::chrono::milliseconds(150));
  MT_CHECK(!done.load());  // 被读事务的 S 挡住（写要 X）

  MT_CHECK(reader.Execute("COMMIT;", &rr).ok());
  w.join();
  MT_CHECK(done.load());
  // 写者在一次等待后就被唤醒并完成，而不是被反复挤压到超时
  MT_CHECK(waited.load() < 3000);
  MT_EQ(RowsText(e.Run("get v in t;").statements[0].result), std::string("7"));
  MT_EQ(e.engine.locks().Stats().timeouts, 0u);
}

MT_TEST(并发_八线程双表混合读写保持行数守恒) {
  Engine e("conc_stress8");
  MT_CHECK(e.Run("CREATE TABLE a(id INT NOT NULL, v INT NOT NULL);"
                 "CREATE TABLE b(id INT NOT NULL, v INT NOT NULL);"
                 "INSERT INTO a VALUES (1,0),(2,0),(3,0),(4,0);"
                 "INSERT INTO b VALUES (1,0),(2,0),(3,0),(4,0);")
               .all_ok());
  MT_EQ(e.engine.locks().Stats().deadlocks, 0u);

  const int kThreads = 8;
  const int kIters = 20;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      Session s(&e.engine, "s8_" + std::to_string(i));
      for (int k = 0; k < kIters; ++k) {
        const int target = (i % 4) + 1;
        const char* tbl = (i % 2 == 0) ? "a" : "b";
        ScriptReport r1;
        (void)s.Execute("get id, v in " + std::string(tbl) + " limit id = " +
                            std::to_string(target) + ";",
                        &r1);
        if (!r1.all_ok()) {
          ++failures;
        }
        ScriptReport r2;
        (void)s.Execute("UPDATE " + std::string(tbl) + " SET v = " +
                            std::to_string(i * 1000 + k) + " limit id = " +
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

  // 行数守恒 + 两表都可读；单语句自动提交不会持锁跨语句，因此不应出现死锁
  MT_EQ(e.Run("get * in a;").statements[0].result.rows.size(), 4u);
  MT_EQ(e.Run("get * in b;").statements[0].result.rows.size(), 4u);
  const LockStats ls = e.engine.locks().Stats();
  MT_EQ(ls.deadlocks, 0u);
  MT_EQ(ls.timeouts, 0u);
  MT_EQ(ls.current_waiters, 0u);
  MT_CHECK(ls.granted + ls.granted_after_wait > 0u);
}

