#pragma once
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include "value.h"

namespace cella::storage {

// ── 一行记录：按顺序的列值集合 ──────────────────────────────
class Record {
 public:
  Record() = default;
  Record(std::initializer_list<Value> vals) : values_(vals) {}

  void AddValue(const Value& v) { values_.push_back(v); }
  void AddValue(Value&& v) { values_.push_back(std::move(v)); }

  size_t value_count() const { return values_.size(); }
  const Value& value(size_t i) const { return values_[i]; }
  const std::vector<Value>& values() const { return values_; }

  std::string ToString() const;

 private:
  std::vector<Value> values_;
};

}  // namespace cella::storage
