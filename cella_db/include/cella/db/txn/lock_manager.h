// lock_manager.h —— 表级锁管理器：严格两阶段封锁（Strict 2PL）+ 死锁检测。
//
// 设计取舍（记录于 docs/INTEGRATION.md）：
//   * 粒度 = 表。存储层的删除是「墓碑」语义、且 IStorage 不暴露行级锁钩子，
//     行级锁需要额外维护 Rid → 锁 映射，收益与复杂度不成比例，故取表级。
//   * 模式 = S（共享，读）/ X（排他，写）。加锁请求冲突时排队等待。
//   * 释放 = 只在事务提交/回滚时统一释放（严格 2PL），因此不会出现
//     「读已提交但锁已放」造成的级联回滚，隔离级别等价于可串行化（表粒度）。
//   * 升级 = 持 S 再请 X 时按「等待所有其它持有者」处理；两个事务互相升级
//     会形成等待环，由死锁检测选出牺牲者。
//
// 死锁检测：不使用缓存的等待图，而是在持锁状态下由「当前锁表 + 未满足的请求」
// 实时推导等待边，再做环检测。这样不会因边过期产生误判（假死锁）。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cella/db/common/db_status.h"

namespace cella::db {

using txn_id_t = uint64_t;
constexpr txn_id_t kInvalidTxnId = 0;

enum class LockMode { kShared, kExclusive };
const char* ToString(LockMode m);

class LockManager {
 public:
  explicit LockManager(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  ~LockManager();
  LockManager(const LockManager&) = delete;
  LockManager& operator=(const LockManager&) = delete;

  // 申请锁：成功返回 OK；形成等待环返回 kDeadlock；超时返回 kLockConflict。
  DbStatus Acquire(txn_id_t txn, const std::string& resource, LockMode mode);

  // 释放该事务持有的全部锁并清空其等待状态（提交/回滚时调用）
  void ReleaseAll(txn_id_t txn);

  bool HoldsAny(txn_id_t txn) const;
  size_t WaiterCount() const;
  size_t DeadlockCount() const;
  void SetTimeout(std::chrono::milliseconds t);

  // 诊断：锁表 / 实时等待边
  std::string Dump() const;
  std::string DumpWaitForGraph() const;

 private:
  struct Pending {
    std::string resource;
    LockMode mode = LockMode::kShared;
  };
  struct Entry {
    std::set<txn_id_t> shared;
    txn_id_t exclusive = kInvalidTxnId;
    std::set<txn_id_t> waiters;  // 诊断用
    std::condition_variable cv;
  };

  Entry* GetOrCreateLocked(const std::string& resource);
  bool CompatibleLocked(const Entry& e, txn_id_t txn, LockMode mode) const;
  void GrantLocked(Entry* e, txn_id_t txn, LockMode mode);
  void ForfeitLocked(Entry* e, txn_id_t txn);
  std::set<txn_id_t> BlockersLocked(txn_id_t txn) const;
  bool FindCycleLocked(txn_id_t start, txn_id_t cur, std::set<txn_id_t>* on_path,
                       std::vector<txn_id_t>* path) const;

  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
  std::map<txn_id_t, std::set<std::string>> held_;
  std::map<txn_id_t, Pending> pending_;  // 尚未满足的请求
  std::chrono::milliseconds timeout_;
  size_t deadlock_count_ = 0;
};

}  // namespace cella::db
