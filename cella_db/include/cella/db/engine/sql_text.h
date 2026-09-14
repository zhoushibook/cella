// sql_text.h —— SQL 文本工具：按顶层分号切分语句 + 事务控制语句识别。
//
// 为什么要自己切分？
//   编译器的输入契约是「完整 SQL 文本」，它不返回每条语句的字节区间。
//   而数据库层需要「逐条语句」：逐条编译（让 DDL 立即对后续语句可见）、
//   逐条执行并汇报结果、逐条计耗时，还要支持交互式 REPL 的「攒够一条再执行」。
//   故本模块做一个只认字符串/注释边界的轻量切分器，仅供分层调度使用，
//   语法正确性仍完全由编译器判定。
#pragma once

#include <string>
#include <vector>

namespace cella::db {

struct SqlStatement {
  std::string text;      // 原始文本（含结尾分号；未闭合时为尾段）
  int line = 1;          // 首个有效字符位置（用于诊断定位）
  int col = 1;
  bool terminated = false;  // 是否以分号结束
};

// 按顶层 ';' 切分。正确处理 '...'（含 '' 转义）、-- 行注释、/* */ 块注释。
// 只含空白/注释的尾段不会产生条目。
std::vector<SqlStatement> SplitSqlStatements(const std::string& sql);

// 是否为事务控制语句。命中时 keyword 返回大写关键字
// （BEGIN / COMMIT / ROLLBACK / END / START）。
bool IsTxnControl(const std::string& stmt_text, std::string* keyword);

// 是否为数据库控制语句（CREATE DATABASE / DROP DATABASE / USE / SHOW DATABASES /
// SHOW INDEXES）。与事务控制一样在会话层拦截，不进编译器。识别必须匹配前两个词
// （CREATE|DROP + DATABASE），否则会误吞 CREATE TABLE。命中时 kind 返回上述之一；
// arg 返回库名原文（SHOW DATABASES 无参数；SHOW INDEXES 的可选表名，
// 都为「无参数」时为空串，由调用方解释语义）。
bool IsDatabaseControl(const std::string& stmt_text, std::string* kind, std::string* arg);

// 去掉首尾空白并转大写（诊断用）
std::string TrimUpper(const std::string& s);

}  // namespace cella::db
