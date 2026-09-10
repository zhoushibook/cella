#include "cella/storage/common/types.h"

namespace cella::storage {

const char* ToString(ValueType t) {
  switch (t) {
    case ValueType::kNull:    return "NULL";
    case ValueType::kBool:    return "BOOL";
    case ValueType::kInt32:   return "INT32";
    case ValueType::kInt64:   return "INT64";
    case ValueType::kFloat:   return "FLOAT";
    case ValueType::kDouble:  return "DOUBLE";
    case ValueType::kVarchar: return "VARCHAR";
    case ValueType::kChar:    return "CHAR";
    case ValueType::kDate:    return "DATE";
  }
  return "UNKNOWN";
}

uint16_t ValueSize(ValueType t) {
  switch (t) {
    case ValueType::kBool:   return 1;
    case ValueType::kInt32:  return 4;
    case ValueType::kFloat:  return 4;
    case ValueType::kInt64:  return 8;
    case ValueType::kDouble: return 8;
    case ValueType::kChar:   return 1;   // 定长占位，后续按 schema 长度
    case ValueType::kNull:
    case ValueType::kVarchar:
    case ValueType::kDate:
    default:                 return 0;   // 变长/Null
  }
}

}  // namespace cella::storage
