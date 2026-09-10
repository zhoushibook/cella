// query_result.h —— 语句执行结果（对外统一交付物）。
//
// 所有语句（DDL/DML/DQL）都归约为一个 QueryResult：
//   * 查询类（GET）：columns + rows 有内容，tag 形如 "GET 3"
//   * 变更类（INSERT/DELETE/UPDATE/DROP）：affected 记录影响行数
//   * 定义类（CREATE TABLE）：tag 描述动作
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cella/storage/common/value.h"

namespace cella::db {

// 结果列（表头）
struct ResultColumn {
  std::string name;
};

struct QueryResult {
  std::vector<ResultColumn> columns;
  std::vector<std::vector<storage::Value>> rows;
  size_t affected = 0;      // 受影响行数（DML）
  std::string tag;          // 简短标签，如 "INSERT 0 3"

  bool IsQuery() const { return !columns.empty(); }
  void Clear() {
    columns.clear();
    rows.clear();
    affected = 0;
    tag.clear();
  }

  // 表格化文本（含表头分隔线；空结果只打印表头）
  // 中文按 2 列宽对齐（UTF-8 码点宽度估算）。
  std::string ToText() const;

  // 单行文本（用于 DDL/DML 的一行摘要）
  std::string Summary() const;
};

// UTF-8 显示宽度：ASCII 记 1，CJK 等宽字符记 2，其余非 ASCII 记 1
size_t DisplayWidth(const std::string& utf8);

}  // namespace cella::db
