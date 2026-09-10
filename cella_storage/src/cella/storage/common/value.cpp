#include "cella/storage/common/value.h"

#include <string>

namespace cella::storage {

std::string Value::ToString() const {
  switch (type) {
    case ValueType::kNull:  return "NULL";
    case ValueType::kBool:  return bool_val ? "true" : "false";
    case ValueType::kInt32: return std::to_string(int32_val);
    case ValueType::kInt64: return std::to_string(int64_val);
    case ValueType::kFloat: return std::to_string(float_val);
    case ValueType::kDouble: return std::to_string(double_val);
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate:  return str_val;
  }
  return "?";
}

}  // namespace cella::storage
