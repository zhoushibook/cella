#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include "types.h"

namespace cella::storage {

// ── 单元格值：支持 NULL / BOOL / INT / VARCHAR 等（§8 记录格式的列值）──
struct Value {
  ValueType type = ValueType::kNull;
  bool     bool_val   = false;
  int32_t  int32_val  = 0;
  int64_t  int64_val  = 0;
  float    float_val  = 0.0f;
  double   double_val = 0.0;
  std::string str_val;   // kVarchar / kChar

  static Value Null() { return Value{}; }
  static Value Bool(bool b) { Value v; v.type = ValueType::kBool; v.bool_val = b; return v; }
  static Value Int(int32_t x) { Value v; v.type = ValueType::kInt32; v.int32_val = x; return v; }
  static Value BigInt(int64_t x) { Value v; v.type = ValueType::kInt64; v.int64_val = x; return v; }
  static Value Float(float x) { Value v; v.type = ValueType::kFloat; v.float_val = x; return v; }
  static Value Double(double x) { Value v; v.type = ValueType::kDouble; v.double_val = x; return v; }
  static Value Varchar(std::string s) {
    Value v;
    v.type = ValueType::kVarchar;
    v.str_val = std::move(s);
    return v;
  }

  bool IsNull() const { return type == ValueType::kNull; }
  std::string ToString() const;
};

}  // namespace cella::storage
