#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>
#include "types.h"

namespace cella::storage {

// ── 列定义 ─────────────────────────────────────────────────
struct Column {
  std::string name;
  ValueType type = ValueType::kInt32;
  uint16_t max_len = 0;   // VARCHAR/CHAR 最大长度（定长类型忽略）
};

// ── 表结构：有序列集合 ──────────────────────────────────────
class Schema {
 public:
  Schema() = default;
  Schema(std::initializer_list<Column> cols) : columns_(cols) {}

  void AddColumn(const std::string& name, ValueType type, uint16_t max_len = 0);
  size_t column_count() const { return columns_.size(); }
  const Column& column(size_t i) const { return columns_[i]; }
  const std::vector<Column>& columns() const { return columns_; }

 private:
  std::vector<Column> columns_;
};

}  // namespace cella::storage
