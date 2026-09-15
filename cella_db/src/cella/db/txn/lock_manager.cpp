#include "cella/db/txn/lock_manager.h"

#include <sstream>
#include <utility>

#include "cella/db/common/db_logger.h"

namespace cella::db {

const char* ToString(LockMode m) { return m == LockMode::kShared ? "S" : "X"; }

LockManager::LockManager(std::chrono::milliseconds timeout) : timeout_(timeout) {}

LockManager::~LockManager() = default;

// ── 公平调度辅助 ───────────────────────────────────────────────

bool LockManager::ContainsWaiter(const Entry& e, txn_id_t txn) {
  for (txn_id_t w : e.waiters) {
    if (w == txn) {
      return true;
    }
  }
  return false;
}

void LockManager::EraseWaiter(Entry* e, txn_id_t txn) {
  for (auto it = e->waiters.begin(); it != e->waiters.end(); ++it) {
    if (*it == txn) {
      e->waiters.erase(it);
      return;
    }
  }
}

// 已经持有满足本次请求的锁 → 属于重入（重复申请 / 同一事务再次访问同一张表）。
// 必须短路放行：否则「队首不是它」的公平规则会把重入判成冲突，把事务自己卡死。
bool LockManager::AlreadyHoldsLocked(const Entry& e, txn_id_t txn, LockMode mode) {
  if (e.exclusive == txn) {
    return true;  // 已持 X：任何请求都已被满足
  }
  return mode == LockMode::kShared && e.shared.count(txn) != 0;
}

// 公平性：队列非空且队首不是它 → 必须排队，哪怕相容性上可以立即满足。
// 这条规则消除了写饥饿：读者不断到来时，早就在等的写者仍然排在前面。
bool LockManager::MustQueueLocked(const Entry& e, txn_id_t txn) {
  if (e.waiters.empty()) {
    return false;
  }
  return e.waiters.front() != txn;
}

// ── 锁表原语 ───────────────────────────────────────────────────

LockManager::Entry* LockManager::GetOrCreateLocked(const std::string& resource) {
  auto it = entries_.find(resource);
  if (it == entries_.end()) {
    it = entries_.emplace(resource, std::make_unique<Entry>()).first;
  }
  return it->second.get();
}

bool LockManager::CompatibleLocked(const Entry& e, txn_id_t txn, LockMode mode) const {
  if (e.exclusive != kInvalidTxnId && e.exclusive != txn) {
    return false;  // 已有他人持 X
  }
  if (mode == LockMode::kShared) {
    return true;  // S 与 S 相容
  }
  // X：要求没有其它共享持有者（自己持有的 S 可升级）
  for (txn_id_t s : e.shared) {
    if (s != txn) {
      return false;
    }
  }
  return true;
}

void LockManager::GrantLocked(Entry* e, txn_id_t txn, LockMode mode) {
  if (mode == LockMode::kExclusive) {
    e->exclusive = txn;
    e->shared.erase(txn);
  } else if (e->exclusive != txn) {
    e->shared.insert(txn);
  }
  EraseWaiter(e, txn);
}

void LockManager::ForfeitLocked(Entry* e, txn_id_t txn) {
  e->shared.erase(txn);
  if (e->exclusive == txn) {
    e->exclusive = kInvalidTxnId;
  }
  EraseWaiter(e, txn);
}

std::set<txn_id_t> LockManager::BlockersLocked(txn_id_t txn) const {
  std::set<txn_id_t> out;
  const auto pit = pending_.find(txn);
  if (pit == pending_.end()) {
    return out;  // 没有未满足的请求 → 不可能参与死锁环
  }
  const auto eit = entries_.find(pit->second.resource);
  if (eit == entries_.end()) {
    return out;
  }
  const Entry& e = *eit->second;
  if (e.exclusive != kInvalidTxnId && e.exclusive != txn) {
    out.insert(e.exclusive);
  }
  if (pit->second.mode == LockMode::kExclusive) {
    for (txn_id_t s : e.shared) {
      if (s != txn) {
        out.insert(s);
      }
    }
  }
  // 公平调度带来的第二类阻塞：我排在 FIFO 队列里，前面的等待者不拿到锁我就动不了。
  // 不把这条边算进来，「持 S 者想升级 X，同时另一个 X 已排在它前面」这种互等
  // 就会漏判 —— 表现为双方都干等到 5s 超时，而不是立刻选出牺牲者。
  for (txn_id_t w : e.waiters) {
    if (w == txn) {
      break;  // 只算排在我前面的
    }
    out.insert(w);
  }
  return out;
}

bool LockManager::FindCycleLocked(txn_id_t start, txn_id_t cur, std::set<txn_id_t>* on_path,
                                 std::vector<txn_id_t>* path) const {
  path->push_back(cur);
  on_path->insert(cur);
  for (txn_id_t h : BlockersLocked(cur)) {
    if (h == start) {
      path->push_back(h);
      return true;
    }
    // 只有「正在等待」的事务才可能成为环的一环
    if (on_path->count(h) == 0 && pending_.count(h) != 0) {
      if (FindCycleLocked(start, h, on_path, path)) {
        return true;
      }
    }
  }
  path->pop_back();
  on_path->erase(cur);
  return false;
}

DbStatus LockManager::Acquire(txn_id_t txn, const std::string& resource, LockMode mode) {
  if (txn == kInvalidTxnId) {
    return DbStatus::Error(DbCode::kInternal, "加锁请求缺少事务号");
  }
  std::unique_lock<std::mutex> lk(mutex_);
  Entry* e = GetOrCreateLocked(resource);

  bool waited_before = false;  // 本次请求是否已经排过队（用于区分「直接授予」与「等待后授予」）
  auto grant = [&]() {
    GrantLocked(e, txn, mode);
    held_[txn].insert(resource);
    pending_.erase(txn);
    if (waited_before) {
      ++stats_.granted_after_wait;
    } else {
      ++stats_.granted;
    }
    DbLogDebug(logcat::kLock, "授予 " + std::string(ToString(mode)) + " 锁 txn=" +
                                  std::to_string(txn) + " 资源=" + resource +
                                  (waited_before ? "（等待后）" : ""));
    return DbStatus::Ok();
  };

  for (;;) {
    // ① 重入 / 重复申请：已经持有满足本次请求的锁 → 直接放行（不走公平队列）
    if (AlreadyHoldsLocked(*e, txn, mode)) {
      return grant();
    }
    // ② 相容且未被公平规则挡住（无等待者，或它就是队首）→ 立即授予
    if (!MustQueueLocked(*e, txn) && CompatibleLocked(*e, txn, mode)) {
      return grant();
    }

    // ③ 排队：记录未满足请求（同一次请求重复入队时不重复刷新排队序号）
    Pending p;
    p.resource = resource;
    p.mode = mode;
    pending_[txn] = p;
    if (!ContainsWaiter(*e, txn)) {
      e->waiters.push_back(txn);
      ++stats_.waited;
      waited_before = true;
    }

    // 推导等待边 → 环检测（牺牲者 = 当前请求者，与既有语义一致）
    std::set<txn_id_t> on_path;
    std::vector<txn_id_t> path;
    if (FindCycleLocked(txn, txn, &on_path, &path)) {
      pending_.erase(txn);
      EraseWaiter(e, txn);
      ++deadlock_count_;
      ++stats_.deadlocks;
      std::ostringstream os;
      os << "检测到死锁，事务 txn=" << txn << " 被选为牺牲者；等待环: ";
      for (size_t i = 0; i < path.size(); ++i) {
        os << (i ? " -> " : "") << path[i];
      }
      const std::string msg = os.str();
      DbLogWarn(logcat::kLock, msg);
      return DbStatus::Error(DbCode::kDeadlock, msg);
    }

    DbLogDebug(logcat::kLock, "等待 " + std::string(ToString(mode)) + " 锁 txn=" +
                                  std::to_string(txn) + " 资源=" + resource);
    const auto t0 = std::chrono::steady_clock::now();
    const std::cv_status st = e->cv.wait_for(lk, timeout_);
    stats_.wait_us_total += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    if (st == std::cv_status::timeout) {
      // 超时前再抢一次（期间可能刚好被释放），否则放弃并清掉等待状态
      if (AlreadyHoldsLocked(*e, txn, mode) ||
          (!MustQueueLocked(*e, txn) && CompatibleLocked(*e, txn, mode))) {
        return grant();
      }
      pending_.erase(txn);
      EraseWaiter(e, txn);
      ++stats_.timeouts;
      const std::string msg = "等待锁超时: txn=" + std::to_string(txn) + " 资源=" + resource;
      DbLogWarn(logcat::kLock, msg);
      return DbStatus::Error(DbCode::kLockConflict, msg);
    }
    // 被唤醒：重新评估（可能已被释放，也可能队首换了人或被别人抢先）
  }
}

void LockManager::ReleaseAll(txn_id_t txn) {
  std::unique_lock<std::mutex> lk(mutex_);
  const auto hit = held_.find(txn);
  if (hit != held_.end()) {
    for (const std::string& res : hit->second) {
      const auto eit = entries_.find(res);
      if (eit != entries_.end()) {
        ForfeitLocked(eit->second.get(), txn);
      }
    }
    held_.erase(hit);
  }
  pending_.erase(txn);
  // 该事务可能正排在某条资源的 FIFO 队里（例如它作为死锁牺牲者被别的线程回滚）。
  // 队列里留下「永远不会再来的事务」会把队首堵死，后来的等待者全部干等到超时，
  // 所以这里必须把它从所有等待队列里摘掉。
  for (auto& kv : entries_) {
    EraseWaiter(kv.second.get(), txn);
  }
  // 释放可能同时唤醒多个资源上的等待者；表数量有限，逐个通知即可
  for (auto& kv : entries_) {
    kv.second->cv.notify_all();
  }
  // 清理已无人持有且无等待者的空条目（避免诊断输出堆一堆空行）。
  // 安全性：等待者把自己的 txn 记进 Entry::waiters，而这里的整个循环都在
  // 互斥量内进行，因此不存在「刚删掉条目、别的线程还拿着它的 Entry*」的窗口。
  for (auto it = entries_.begin(); it != entries_.end();) {
    const Entry& e = *it->second;
    if (e.shared.empty() && e.exclusive == kInvalidTxnId && e.waiters.empty()) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  DbLogDebug(logcat::kLock, "释放全部锁 txn=" + std::to_string(txn));
}

bool LockManager::HoldsAny(txn_id_t txn) const {
  std::unique_lock<std::mutex> lk(mutex_);
  const auto it = held_.find(txn);
  return it != held_.end() && !it->second.empty();
}

size_t LockManager::WaiterCount() const {
  std::unique_lock<std::mutex> lk(mutex_);
  return pending_.size();
}

size_t LockManager::DeadlockCount() const {
  std::unique_lock<std::mutex> lk(mutex_);
  return deadlock_count_;
}

void LockManager::SetTimeout(std::chrono::milliseconds t) {
  std::unique_lock<std::mutex> lk(mutex_);
  timeout_ = t;
}

std::chrono::milliseconds LockManager::Timeout() const {
  std::unique_lock<std::mutex> lk(mutex_);
  return timeout_;
}

LockStats LockManager::Stats() const {
  std::unique_lock<std::mutex> lk(mutex_);
  LockStats s = stats_;
  s.deadlocks = static_cast<uint64_t>(deadlock_count_);
  s.current_waiters = pending_.size();
  size_t held = 0;
  for (const auto& kv : held_) {
    held += kv.second.size();
  }
  s.held_locks = held;
  return s;
}

std::string LockManager::StatsText() const {
  std::unique_lock<std::mutex> lk(mutex_);
  LockStats s = stats_;
  s.deadlocks = static_cast<uint64_t>(deadlock_count_);
  s.current_waiters = pending_.size();
  size_t held = 0;
  for (const auto& kv : held_) {
    held += kv.second.size();
  }
  s.held_locks = held;

  uint64_t waiters_touched = s.granted_after_wait + s.timeouts;
  const double avg_ms =
      waiters_touched == 0
          ? 0.0
          : (static_cast<double>(s.wait_us_total) / 1000.0) / static_cast<double>(waiters_touched);

  std::ostringstream os;
  os << "并发/锁指标（表级 S/X 锁 + 公平 FIFO 调度）:\n";
  os << "  锁超时阈值        : " << timeout_.count() << " ms\n";
  os << "  当前持有锁        : " << held << " 条（事务 × 资源）\n";
  os << "  当前等待中的事务  : " << s.current_waiters << "\n";
  os << "  直接授予          : " << s.granted << " 次\n";
  os << "  进入等待          : " << s.waited << " 次\n";
  os << "  等待后授予        : " << s.granted_after_wait << " 次\n";
  os << "  等待超时放弃      : " << s.timeouts << " 次\n";
  os << "  死锁检出/牺牲     : " << s.deadlocks << " 次\n";
  os << "  平均等待时长      : " << avg_ms << " ms\n";
  return os.str();
}

std::string LockManager::Dump() const {
  std::unique_lock<std::mutex> lk(mutex_);
  std::ostringstream os;
  os << "锁表（资源 = 持有者 / 等待者）:\n";
  if (entries_.empty()) {
    os << "  (空)\n";
  }
  for (const auto& kv : entries_) {
    os << "  " << kv.first << " : ";
    if (kv.second->exclusive != kInvalidTxnId) {
      os << "[X txn=" << kv.second->exclusive << "]";
    }
    for (txn_id_t s : kv.second->shared) {
      os << "[S txn=" << s << "]";
    }
    if (!kv.second->waiters.empty()) {
      // 保持「等待: a,b,c」既有格式：客户端的结构化解析按逗号切分，
      // 而这里按 FIFO 顺序输出，因此解析结果天然就是队列顺序（队首在前）。
      os << " 等待: ";
      bool first = true;
      for (txn_id_t w : kv.second->waiters) {
        os << (first ? "" : ",") << w;
        first = false;
      }
    }
    os << "\n";
  }
  return os.str();
}

std::string LockManager::DumpWaitForGraph() const {
  std::unique_lock<std::mutex> lk(mutex_);
  std::ostringstream os;
  os << "等待图（txn -> 阻塞它的 txn）:\n";
  if (pending_.empty()) {
    os << "  (无等待)\n";
  }
  for (const auto& kv : pending_) {
    os << "  " << kv.first << " -> ";
    const std::set<txn_id_t> b = BlockersLocked(kv.first);
    if (b.empty()) {
      os << "(无)";
    }
    bool first = true;
    for (txn_id_t t : b) {
      os << (first ? "" : ",") << t;
      first = false;
    }
    os << "   [" << std::string(ToString(kv.second.mode)) << " " << kv.second.resource << "]\n";
  }
  return os.str();
}

}  // namespace cella::db
