// db_status.h —— 数据库层统一返回状态与错误码（整合层自己的诊断空间）。
//
// 与编译器、存储层的关系：
//   * 编译器用 std::vector<CELLA_Error>（阶段 + 错误码 + 位置）报告前端错误；
//   * 存储层用 [[nodiscard]] Status（StatusCode + message）报告物理错误；
//   * 本层把二者收敛为 DbStatus：对外只暴露「错误码 + 说明」，并保留来源。
// 错误码分段：DB-5xx 目录/执行，DB-6xx 事务/并发，DB-7xx 会话/CLI。
#pragma once

#include <string>

namespace cella::db {

// ── 错误码（一类错误一个码，便于日志检索与测试断言）──────────
enum class DbCode {
  kOk = 0,

  // —— 前端/编译期（DB-5xx）——
  kSqlError,            // DB-501 词法/语法/语义/计划阶段报错
  kTableNotFound,       // DB-502 表不存在
  kTableExists,         // DB-503 表已存在
  kColumnNotFound,      // DB-504 列不存在
  kTypeMismatch,        // DB-505 值类型与列类型不匹配
  kNotNullViolation,    // DB-506 向 NOT NULL 列写入 NULL
  kValueCountMismatch,  // DB-507 INSERT 值个数与列个数不一致
  kValueTooLong,        // DB-508 字符串超出列声明长度
  kUnknownColumn,       // DB-509 目录元数据与存储 schema 不一致
  kRecordTooLarge,      // DB-510 记录超出单页容量
  kDivisionByZero,      // DB-511 表达式求值除零
  kSystemTableProtected,// DB-512 系统表禁止修改（只读）
  kDatabaseTxnActive,   // DB-513 数据库级操作前须结束当前事务
  kDatabaseError,       // DB-514 数据库不存在/已存在/名字非法
  kDatabaseProtected,   // DB-515 目标数据库不可删除（当前库/启动库）
  kPrimaryKeyViolation, // DB-516 主键冲突（唯一性被破坏）

  // —— 存储层透传（DB-5xx）——
  kStorageError,        // DB-520 存储引擎返回失败（message 携带原始码）

  // —— 事务/并发（DB-6xx）——
  kNoActiveTxn,         // DB-601 没有活动事务时执行了需事务的操作
  kTxnAlreadyActive,    // DB-602 重复 BEGIN
  kTxnAborted,          // DB-603 事务已被中止（牺牲者/显式回滚）
  kDeadlock,            // DB-604 检测到死锁，本事务被选为牺牲者
  kLockConflict,        // DB-605 加锁冲突（非阻塞尝试失败）

  // —— 目录/会话/CLI（DB-7xx）——
  kCatalogError,        // DB-701 目录文件读写/格式错误
  kSessionError,        // DB-702 会话状态错误（如未在事务中 COMMIT）
  kNotImplemented,      // DB-703 未实现的特性
  kInternal,            // DB-704 内部不变量被破坏
};

// "DB-501" 形式的错误码文本
const char* ToString(DbCode c);

// ── 统一返回状态 ────────────────────────────────────────────
// [[nodiscard]]：忽略返回值直接触发编译告警（与存储层 Status 一致）。
class [[nodiscard]] DbStatus {
 public:
  DbStatus() = default;
  DbStatus(DbCode code, std::string message);

  static DbStatus Ok() { return DbStatus(); }
  static DbStatus Error(DbCode c, std::string msg) { return DbStatus(c, std::move(msg)); }

  bool ok() const { return code_ == DbCode::kOk; }
  DbCode code() const { return code_; }
  const std::string& message() const { return msg_; }

  // "[DB-502] 表不存在: student"
  std::string ToString() const;

 private:
  DbCode code_ = DbCode::kOk;
  std::string msg_;
};

}  // namespace cella::db
