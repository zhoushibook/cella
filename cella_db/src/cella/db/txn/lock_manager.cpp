#include "cella/db/txn/lock_manager.h"

#include <sstream>

#include "cella/db/common/db_logger.h"

namespace cella::db {

const char* ToString(LockMode m) { return m == LockMode::kShared ? "S" : "X"; }

LockManager::LockManager(std::chrono::milliseconds timeout) : timeout_(timeout) {}

LockManager::~LockManager() = default;

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
  e->waiters.erase(txn);
}

void LockManager::ForfeitLocked(Entry* e, txn_id_t txn) {
  e->shared.erase(txn);
  if (e->exclusive == txn) {
    e->exclusive = kInvalidTxnId;
  }
  e->waiters.erase(txn);
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

  for (;;) {
    if (CompatibleLocked(*e, txn, mode)) {
      GrantLocked(e, txn, mode);
      held_[txn].insert(resource);
      pending_.erase(txn);
      DbLogDebug(logcat::kLock, "授予 " + std::string(ToString(mode)) + " 锁 txn=" +
                                    std::to_string(txn) + " 资源=" + resource);
      return DbStatus::Ok();
    }

    // 记录未满足请求 → 推导等待边 → 环检测
    Pending p;
    p.resource = resource;
    p.mode = mode;
    pending_[txn] = p;

    std::set<txn_id_t> on_path;
    std::vector<txn_id_t> path;
    if (FindCycleLocked(txn, txn, &on_path, &path)) {
      pending_.erase(txn);
      e->waiters.erase(txn);
      ++deadlock_count_;
      std::ostringstream os;
      os << "检测到死锁，事务 txn=" << txn << " 被选为牺牲者；等待环: ";
      for (size_t i = 0; i < path.size(); ++i) {
        os << (i ? " -> " : "") << path[i];
      }
      const std::string msg = os.str();
      DbLogWarn(logcat::kLock, msg);
      return DbStatus::Error(DbCode::kDeadlock, msg);
    }

    e->waiters.insert(txn);
    DbLogDebug(logcat::kLock, "等待 " + std::string(ToString(mode)) + " 锁 txn=" +
                                  std::to_string(txn) + " 资源=" + resource);
    const std::cv_status st = e->cv.wait_for(lk, timeout_);
    if (st == std::cv_status::timeout) {
      if (CompatibleLocked(*e, txn, mode)) {
        GrantLocked(e, txn, mode);
        held_[txn].insert(resource);
        pending_.erase(txn);
        return DbStatus::Ok();
      }
      pending_.erase(txn);
      e->waiters.erase(txn);
      const std::string msg = "等待锁超时: txn=" + std::to_string(txn) + " 资源=" + resource;
      DbLogWarn(logcat::kLock, msg);
      return DbStatus::Error(DbCode::kLockConflict, msg);
    }
    // 被唤醒：重新评估相容性（可能已被释放，也可能被别的等待者抢先）
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
