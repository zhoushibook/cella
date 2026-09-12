// row_set.h —— 执行期内元组集合（引擎组内存内运算的通用中间结果）。
//
// 存储层只提供原始表访问，Filter/Project/Join/Sort/... 一律在本层完成。
// 为此需要一个既能装值、
// 又能按 SQL 规则解析列引用（含表限定名）的中间表示。
//
//   fields[i] 描述第 i 列的来源（限定符 + 列名）
//   rows[r][i] 是对应单元格值
//
// 列解析规则（与编译器 SEM-303/308 的语义保持一致）：
//   * 限定引用 "t.c" → 只匹配 qualifier == t（大小写不敏感）且 name == c
//   * 裸引用  "c"    → 匹配任意 qualifier 的 name == c（重名由语义阶段拦下）
#pragma once

#include <string>
#include <vector>

#include "cella/storage/common/value.h"

namespace cella::db
{

  // 一列的来源描述
  struct FieldRef
  {
    std::string qualifier; // 表名或别名；Project 产生的列无限定符
    std::string name;      // 列名（原始拼写）

    // 结果的表头显示名（不含限定符，保持 SQL 直觉）
    std::string Display() const { return name; }
  };

  // 值的有序表
  struct RowSet
  {
    std::vector<FieldRef> fields;
    std::vector<std::vector<storage::Value>> rows;

    size_t arity() const { return fields.size(); }
    size_t row_count() const { return rows.size(); }
    bool empty() const { return rows.empty(); }

    void Clear()
    {
      fields.clear();
      rows.clear();
    }

    // 解析列引用 → 字段下标；未找到返回 -1。
    // qualifier 为空表示裸引用；name 的比较大小写不敏感。
    int Resolve(const std::string &qualifier, const std::string &name) const;

    // 追加一行（列数应等于 arity）
    void AddRow(std::vector<storage::Value> row) { rows.push_back(std::move(row)); }
  };

} // namespace cella::db
