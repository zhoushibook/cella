// wal_manager.h —— WAL 的落地管理：追加、刷盘、顺序读取、存盘后压缩。
//
// 职责边界（刻意保持很窄）：
//   * 本类只管**字节怎么落盘**，不理解事务语义、不知道什么叫提交；
//   * 「什么时候必须刷」由上层（TxnManager / DbEngine）按 WAL 规则调用；
//   * 「刷出去的日志怎么解释」交给 RecoveryManager（recovery_manager.h）。
//
// 刷盘语义（重要，别误会）：
//   Flush() 只保证把缓冲区交给操作系统（write() 成功），**不做 fsync**。
//   这足以抵御「进程被强杀」（OS 页缓冲还在），但扛不住「机器掉电」。
//   教学系统只承诺前者，机器掉电需要额外加 fsync —— 见 WAL_RECOVERY.md。
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "cella/db/common/db_status.h"
#include "cella/db/wal/wal_types.h"

namespace cella::db::wal
{

  // ── 打开结果（调用方据此决定 LSN 起点与是否需要恢复）────────
  struct OpenResult
  {
    lsn_t next_lsn = kFirstLsn;   // 下一条记录将拿到的 LSN
    size_t records = 0;           // 文件中读到的完整记录数
    size_t truncated_bytes = 0;   // 被丢弃的坏尾字节数（0 = 日志干净收尾）
    bool created = false;         // 文件是本次新建的
    bool legacy_archived = false; // 发现旧版文本 journal.log 并已改名归档
  };

  class WalManager
  {
  public:
    WalManager();
    ~WalManager();
    WalManager(const WalManager &) = delete;
    WalManager &operator=(const WalManager &) = delete;

    // 打开（必要时创建）日志文件并扫描已有记录。
    // 扫描的目的有两个：① 取回 LSN 计数；② 发现坏尾（上次崩溃的痕迹）并截断。
    // 发现旧版纯文本 journal.log（没有本格式的 magic）→ 改名成 journal.log.legacy
    // 后重新开始，这就是 P2.1 说的「升级 journal.log」。
    DbStatus Open(const std::string &path, OpenResult *out);
    void Close(); // 只关文件，**不刷**未落盘的缓冲（异常退出就该丢）
    bool opened() const { return opened_; }
    const std::string &path() const { return path_; }

    // 追加一条记录：分配 LSN、编码、写入缓冲。返回分配到的 LSN。
    // flush_each_record_ 为真时立刻刷盘 —— 这是「未提交事务的脏页也可能被缓冲池
    // 淘汰到磁盘」这一现实下，保证原子性唯一的稳妥做法（见 WAL_RECOVERY.md）。
    lsn_t Append(const WalRecord &rec);
    // 把缓冲交给操作系统。提交点、存盘点、干净关闭前都必须调用。
    void Flush();
    // 在 Flush 基础上要求操作系统把 WAL 文件同步到稳定存储。
    DbStatus FlushDurable();

    lsn_t next_lsn() const { return next_lsn_; }
    lsn_t durable_lsn() const { return durable_lsn_; } // 已落盘的最大 LSN
    size_t buffered_bytes() const { return buffer_.size(); }
    size_t appended_count() const { return appended_count_; }
    uint64_t flush_count() const { return flush_count_; }

    // 顺序读出全部有效记录（供恢复期分析阶段使用）
    DbStatus ReadAll(std::vector<WalRecord> *out) const;

    // 存盘点后的日志压缩：只保留 lsn >= keep_from 的记录，其余物理删掉。
    // 走「写临时文件 → 刷盘 → 删除旧文件 → 改名」的步骤，避免压到一半崩溃时
    // 旧日志已经没了、新的还没就位。
    DbStatus Compact(lsn_t keep_from, size_t *kept, size_t *dropped);

    // 全部丢弃（干净关闭 / 恢复完成并落盘之后调用）
    DbStatus TruncateAll();

    // 每记录刷盘开关。关掉会快一些，但「未提交事务被淘汰到磁盘」时恢复无据可依。
    void set_flush_each_record(bool b) { flush_each_record_ = b; }
    bool flush_each_record() const { return flush_each_record_; }

    std::string Describe() const;

  private:
    // 扫描整个文件：读出完整记录，并给出「好数据的结束偏移」与坏尾字节数
    DbStatus Scan(std::vector<WalRecord> *out, size_t *valid_end, size_t *truncated) const;
    DbStatus WriteFileHeader();
    DbStatus SyncFile();

    std::string path_;
    std::ofstream out_;
    bool opened_ = false;
    bool flush_each_record_ = true;
    lsn_t next_lsn_ = kFirstLsn;
    lsn_t durable_lsn_ = kInvalidLsn;
    std::string buffer_; // 待落盘字节（Flush 时一次 write 出去）
    size_t appended_count_ = 0;
    uint64_t flush_count_ = 0;
    // 执行器在 storage_mutex_ 内追加日志、事务管理器在自己的 mutex_ 内提交，
    // 两条路径会并发 → 日志写入必须自己串行化（用递归锁：Compact 里会连着调 Flush+Scan）。
    mutable std::recursive_mutex mu_;
  };

} // namespace cella::db::wal
