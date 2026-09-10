#include "cella/db/common/value_bridge.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace cella::db {
namespace {

bool IsNumericType(ValueType t) {
  return t == ValueType::kInt32 || t == ValueType::kInt64 || t == ValueType::kFloat ||
         t == ValueType::kDouble;
}

bool IsTextType(ValueType t) {
  return t == ValueType::kVarchar || t == ValueType::kChar || t == ValueType::kDate;
}

double AsDouble(const Value& v) {
  switch (v.type) {
    case ValueType::kInt32:  return static_cast<double>(v.int32_val);
    case ValueType::kInt64:  return static_cast<double>(v.int64_val);
    case ValueType::kFloat:  return static_cast<double>(v.float_val);
    case ValueType::kDouble: return v.double_val;
    default:                 return 0.0;
  }
}

// 无论物理类型，取「文本形态」用于兜底比较
std::string AsText(const Value& v) { return v.ToString(); }

// 数值是否可无损放进 int32
bool FitsInt32(double x) {
  return x >= static_cast<double>(std::numeric_limits<int32_t>::min()) &&
         x <= static_cast<double>(std::numeric_limits<int32_t>::max());
}

}  // namespace

// ─────────────────────────────────────────────────────────────
// 类型桥
// ─────────────────────────────────────────────────────────────

ValueType ToStorageType(cella::CELLA_DataType t) {
  switch (t) {
    case cella::CELLA_DataType::INT:      return ValueType::kInt32;
    case cella::CELLA_DataType::FLOAT:    return ValueType::kFloat;
    case cella::CELLA_DataType::DOUBLE:   return ValueType::kDouble;
    case cella::CELLA_DataType::CHAR:     return ValueType::kChar;
    case cella::CELLA_DataType::VARCHAR:  return ValueType::kVarchar;
    case cella::CELLA_DataType::TEXT:     return ValueType::kVarchar;
    case cella::CELLA_DataType::DATE:     return ValueType::kVarchar;
    case cella::CELLA_DataType::TIME:     return ValueType::kVarchar;
    case cella::CELLA_DataType::DATETIME: return ValueType::kVarchar;
  }
  return ValueType::kVarchar;
}

uint16_t StorageMaxLen(cella::CELLA_DataType t, int declared_len, bool has_len) {
  switch (t) {
    case cella::CELLA_DataType::CHAR:
    case cella::CELLA_DataType::VARCHAR:
      return static_cast<uint16_t>(has_len ? declared_len : 255);  // 与编译器默认一致
    case cella::CELLA_DataType::TEXT:
      return 0;  // 0 = 不限制（受存储层 u16 长度上限约束）
    default:
      return 0;  // 定长类型忽略
  }
}

Schema ToStorageSchema(const cella::CELLA_Table& table) {
  Schema schema;
  for (const auto& c : table.columns) {
    const uint16_t max_len = StorageMaxLen(c.type, c.len, c.len > 0);
    schema.AddColumn(c.name, ToStorageType(c.type), max_len);
  }
  return schema;
}

// ─────────────────────────────────────────────────────────────
// 字面量桥
// ─────────────────────────────────────────────────────────────

bool IsIntegralLiteralText(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (c == '.' || c == 'e' || c == 'E') {
      return false;
    }
  }
  return true;
}

Value LiteralToValue(const cella::CELLA_Expr& e) {
  if (e.kind != cella::CELLA_Expr::Kind::LITERAL) {
    return Value::Null();
  }
  switch (e.lit) {
    case cella::CELLA_LiteralKind::NULL_LIT:
      return Value::Null();
    case cella::CELLA_LiteralKind::BOOL_LIT:
      return Value::Bool(e.boolVal);
    case cella::CELLA_LiteralKind::STRING:
    case cella::CELLA_LiteralKind::DATE:
      return Value::Varchar(e.text);
    case cella::CELLA_LiteralKind::NUMBER:
      break;
  }
  // 整数字面量且落在 int32 内 → 精确的 INT；否则用 DOUBLE 保留小数/溢出
  if (IsIntegralLiteralText(e.text) && FitsInt32(e.num)) {
    return Value::Int(static_cast<int32_t>(e.num));
  }
  return Value::Double(e.num);
}

// ─────────────────────────────────────────────────────────────
// 取值约束
// ─────────────────────────────────────────────────────────────

DbStatus CoerceValue(const Value& in, cella::CELLA_DataType target, uint16_t max_len,
                     bool not_null, Value* out) {
  if (out == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "CoerceValue: out 为空");
  }
  if (in.IsNull()) {
    if (not_null) {
      return DbStatus::Error(DbCode::kNotNullViolation, "列声明为 NOT NULL，不接受 NULL");
    }
    *out = Value::Null();
    return DbStatus::Ok();
  }

  const ValueType src = in.type;
  const bool src_num = IsNumericType(src);
  const bool src_text = IsTextType(src);

  switch (target) {
    case cella::CELLA_DataType::INT: {
      if (!src_num) {
        if (src == ValueType::kBool) {
          *out = Value::Int(in.bool_val ? 1 : 0);
          return DbStatus::Ok();
        }
        return DbStatus::Error(DbCode::kTypeMismatch,
                               "期望整数，实际为 " + std::string(storage::ToString(src)));
      }
      const double x = AsDouble(in);
      if (std::floor(x) != x) {
        return DbStatus::Error(DbCode::kTypeMismatch,
                               "值 " + RenderValue(in) + " 含小数部分，无法存入 INT 列");
      }
      if (!FitsInt32(x)) {
        return DbStatus::Error(DbCode::kTypeMismatch, "值 " + RenderValue(in) + " 超出 INT 范围");
      }
      *out = Value::Int(static_cast<int32_t>(x));
      return DbStatus::Ok();
    }
    case cella::CELLA_DataType::FLOAT: {
      if (!src_num) {
        return DbStatus::Error(DbCode::kTypeMismatch,
                               "期望数值，实际为 " + std::string(storage::ToString(src)));
      }
      *out = Value::Float(static_cast<float>(AsDouble(in)));
      return DbStatus::Ok();
    }
    case cella::CELLA_DataType::DOUBLE: {
      if (!src_num) {
        return DbStatus::Error(DbCode::kTypeMismatch,
                               "期望数值，实际为 " + std::string(storage::ToString(src)));
      }
      *out = Value::Double(AsDouble(in));
      return DbStatus::Ok();
    }
    default:
      break;
  }

  // 字符族与日期/时间族：统一按文本存储
  if (!src_text && src != ValueType::kBool) {
    return DbStatus::Error(DbCode::kTypeMismatch,
                           "期望字符串/日期，实际为 " + std::string(storage::ToString(src)));
  }
  const std::string text = AsText(in);
  if (max_len > 0 && text.size() > max_len) {
    return DbStatus::Error(DbCode::kValueTooLong, "文本长度 " + std::to_string(text.size()) +
                                                      " 超出列声明上限 " +
                                                      std::to_string(max_len));
  }
  *out = Value::Varchar(text);
  return DbStatus::Ok();
}

// ─────────────────────────────────────────────────────────────
// 比较
// ─────────────────────────────────────────────────────────────

int CompareValues(const Value& a, const Value& b, bool* known) {
  if (known != nullptr) {
    *known = true;
  }
  if (a.IsNull() || b.IsNull()) {
    if (known != nullptr) {
      *known = false;  // SQL 三值逻辑：与 NULL 比较结果为 UNKNOWN
    }
    return 0;
  }
  if (IsNumericType(a.type) && IsNumericType(b.type)) {
    const double x = AsDouble(a);
    const double y = AsDouble(b);
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
  }
  if (a.type == ValueType::kBool && b.type == ValueType::kBool) {
    const int x = a.bool_val ? 1 : 0;
    const int y = b.bool_val ? 1 : 0;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
  }
  const std::string x = AsText(a);
  const std::string y = AsText(b);
  const int c = x.compare(y);
  return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
}

bool ValueEquals(const Value& a, const Value& b) {
  // 分组/去重语义：NULL 与 NULL 视为同一组
  if (a.IsNull() && b.IsNull()) {
    return true;
  }
  if (a.IsNull() != b.IsNull()) {
    return false;
  }
  return CompareValues(a, b, nullptr) == 0;
}

// ─────────────────────────────────────────────────────────────
// 渲染
// ─────────────────────────────────────────────────────────────

std::string RenderValue(const Value& v) {
  if (v.IsNull()) {
    return "NULL";
  }
  switch (v.type) {
    case ValueType::kInt32:
      return std::to_string(v.int32_val);
    case ValueType::kInt64:
      return std::to_string(v.int64_val);
    case ValueType::kFloat: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.float_val));
      return std::string(buf);
    }
    case ValueType::kDouble: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%g", v.double_val);
      return std::string(buf);
    }
    case ValueType::kBool:
      return v.bool_val ? "TRUE" : "FALSE";
    default:
      return v.str_val;
  }
}

std::string RenderType(const cella::CELLA_DataType t) {
  return cella::cella_typeName(t);
}

}  // namespace cella::db
