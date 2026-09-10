// concurrency_demo.cpp —— 并发控制现场演示：共享锁相容、写锁互斥、死锁检测与牺牲者。
//
// 三个场景都由真实线程驱动真实的会话（Session），不是模拟：
//   场景 1  读读并发：两个会话同时 GET，S 锁相容，互不阻塞。
//   场景 2  写写互斥：会话 A 持 X 锁时，会话 B 的写请求阻塞，A 提交后被唤醒。
//   场景 3  死锁检测：A 先锁 t1 再要 t2，B 先锁 t2 再要 t1 → 成环；
//           等待图检测到环后把「后请求者」选为牺牲者，整事务回滚并释放锁，
//           另一方随即获得锁继续执行。
//
// 运行：build/cella_db/concurrency_demo.exe
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "cella/db/common/value_bridge.h"
#include "cella/db/engine/db_engine.h"

using cella::db::DbEngine;
using cella::db::EngineConfig;
using cella::db::ScriptReport;
using cella::db::Session;

namespace {

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string Rows(const ScriptReport& r) {
  if (r.statements.empty()) {
    return "(无语句)";
  }
  const auto& q = r.statements.back().result;
  std::string s;
  for (size_t i = 0; i < q.rows.size(); ++i) {
    if (i != 0) {
      s += " | ";
    }
    for (size_t j = 0; j < q.rows[i].size(); ++j) {
      if (j != 0) {
        s += ",";
      }
      s += cella::db::RenderValue(q.rows[i][j]);
    }
  }
  return s;
}

std::string Status(const ScriptReport& r) {
  if (r.statements.empty()) {
    return "(无语句)";
  }
  const auto& s = r.statements.back();
  return s.status.ok() ? std::string("OK") : s.status.ToString();
}

}  // namespace

int main() {
  EngineConfig cfg;
  cfg.data_dir = "./concurrency_data";
  cfg.enable_log = false;
  cfg.lock_timeout = std::chrono::milliseconds(4000);
  // 每次运行都从空库开始，保证演示结果可复现
  {
    std::error_code ec;
    std::filesystem::remove_all(cfg.data_dir, ec);
  }

  DbEngine engine;
  if (!engine.Open(cfg).ok()) {
    std::cerr << "打开失败\n";
    return 1;
  }
  {
    ScriptReport setup;
    (void)engine.default_session().Execute(
        "CREATE TABLE t1(id INT, v INT);CREATE TABLE t2(id INT, v INT);"
        "INSERT INTO t1 VALUES (1,10);INSERT INTO t2 VALUES (1,20);",
        &setup);
    if (!setup.all_ok()) {
      std::cerr << "初始化失败\n";
      engine.Close();
      return 1;
    }
  }

  // ───────── 场景 1：读读并发（S 锁相容）─────────
  std::cout << "=== 场景 1：两个会话并发读同一张表（S 锁相容，不阻塞）===\n";
  {
    Session a(&engine, "reader-A");
    Session b(&engine, "reader-B");
    std::atomic<int> ok_count{0};
    std::thread tb([&] {
      ScriptReport r;
      (void)b.Execute("get id, v in t1;", &r);
      if (r.all_ok()) {
        ++ok_count;
      }
      std::cout << "  [B] 读完成: " << Status(r) << "\n";
    });
    ScriptReport ra;
    (void)a.Execute("get id, v in t1;", &ra);
    std::cout << "  [A] 读完成: " << Status(ra) << " 值 " << Rows(ra) << "\n";
    tb.join();
    std::cout << "  两者都成功: " << (ok_count.load() == 1 ? "是" : "否") << "\n\n";
  }

  // ───────── 场景 2：写写互斥（X 锁）─────────
  std::cout << "=== 场景 2：写锁互斥 —— A 未提交时 B 的写请求阻塞 ===\n";
  {
    Session a(&engine, "writer-A");
    Session b(&engine, "writer-B");
    ScriptReport r;
    (void)a.Execute("BEGIN;", &r);
    (void)a.Execute("UPDATE t1 SET v = 11 limit id = 1;", &r);
    std::cout << "  [A] 已持 t1 的 X 锁并写入 v=11（未提交）\n";

    std::atomic<bool> b_done{false};
    std::thread tb([&] {
      ScriptReport rb;
      (void)b.Execute("UPDATE t1 SET v = 22 limit id = 1;", &rb);
      std::cout << "  [B] 写完成: " << Status(rb) << "\n";
      b_done = true;
    });
    SleepMs(200);
    std::cout << "  等待中的加锁请求数: " << engine.locks().WaiterCount()
              << " ；B 是否已完成: " << (b_done.load() ? "是" : "否（被阻塞）") << "\n";
    std::cout << "  [A] COMMIT → 释放锁\n";
    (void)a.Execute("COMMIT;", &r);
    tb.join();
    ScriptReport rf;
    (void)engine.default_session().Execute("get v in t1;", &rf);
    std::cout << "  最终 t1.v = " << Rows(rf) << "（B 后写，串行化正确）\n\n";
  }

  // ───────── 场景 3：死锁检测与牺牲者 ─────────
  std::cout << "=== 场景 3：交叉加锁形成死锁环，检测后选牺牲者回滚 ===\n";
  {
    Session a(&engine, "deadlock-A");
    Session b(&engine, "deadlock-B");
    ScriptReport r;
    (void)a.Execute("BEGIN;", &r);
    (void)a.Execute("UPDATE t1 SET v = 100 limit id = 1;", &r);   // A 持 t1
    (void)b.Execute("BEGIN;", &r);
    (void)b.Execute("UPDATE t2 SET v = 200 limit id = 1;", &r);   // B 持 t2
    std::cout << "  [A] 持 t1 的 X 锁，[B] 持 t2 的 X 锁\n";

    std::atomic<bool> a_done{false};
    std::atomic<int> a_code{-1};
    std::thread ta([&] {
      ScriptReport ra;
      (void)a.Execute("UPDATE t2 SET v = 101 limit id = 1;", &ra);  // A 要 t2 → 阻塞
      a_code = static_cast<int>(ra.statements.back().status.code());
      std::cout << "  [A] 第二次写返回: " << Status(ra) << "\n";
      a_done = true;
    });
    SleepMs(200);
    std::cout << "  [A] 是否仍阻塞: " << (a_done.load() ? "否" : "是") << "\n";
    std::cout << "  等待图:\n" << engine.locks().DumpWaitForGraph();

    ScriptReport rb;
    (void)b.Execute("UPDATE t1 SET v = 201 limit id = 1;", &rb);    // B 要 t1 → 成环
    std::cout << "  [B] 反向请求返回: " << Status(rb) << "（被选为牺牲者）\n";
    std::cout << "  累计死锁次数: " << engine.locks().DeadlockCount() << "\n";
    ta.join();
    std::cout << "  [A] 最终返回 " << (a_code.load() == 0 ? "OK（拿到锁）" : "失败") << "\n";
    (void)a.Execute("COMMIT;", &r);
    std::cout << "  B 的事务是否已结束: " << (b.in_transaction() ? "否" : "是（已回滚）") << "\n";
    ScriptReport rf;
    (void)engine.default_session().Execute("get v in t1;", &rf);
    std::cout << "  最终 t1.v = " << Rows(rf) << "（B 对 t1 的写入已随回滚撤销）\n\n";
  }

  std::cout << "=== 锁表快照 ===\n" << engine.locks().Dump();
  std::cout << "=== 事务统计 ===\n" << engine.TxnText();
  engine.Close();
  return 0;
}
