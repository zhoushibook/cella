// sql_builder.cpp —— SQL 生成实现（方言事实见头文件注释与 PLAN §11）。
#include "cella/client/api/sql_builder.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace cella::client {

using cella::storage::Value;

namespace {

std::string Upper(const std::string& s) {
  std::string out;
  for (const char c : s) {
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

// 浮点字面量：既短又能精确还原（15→16→17 位依次尝试，与 JSON 侧同一策略）
std::string DoubleLiteral(double v) {
  char buf[48];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) {
      return buf;
    }
  }
  return buf;
}

std::string NumberLiteral(const Value& v) {
  switch (v.type) {
    case cella::storage::ValueType::kInt32: return std::to_string(v.int32_val);
    case cella::storage::ValueType::kInt64: return std::to_string(v.int64_val);
    case cella::storage::ValueType::kFloat: return DoubleLiteral(v.float_val);
    case cella::storage::ValueType::kDouble: return DoubleLiteral(v.double_val);
    default: return "NULL";
  }
}

bool IsNumericDeclared(const std::string& declared) {
  const std::string t = Upper(declared);
  return t == "INT" || t == "INTEGER" || t == "FLOAT" || t == "DOUBLE";
}

bool IsTextDeclared(const std::string& declared) {
  const std::string t = Upper(declared);
  return t == "CHAR" || t == "VARCHAR" || t == "TEXT" || t == "DATE" || t == "TIME" ||
         t == "DATETIME";
}

}  // namespace

bool ValidIdentifier(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const bool ok = c == '_' || std::isdigit(static_cast<unsigned char>(c)) ||
                    std::isalpha(static_cast<unsigned char>(c));
    if (!ok || (i == 0 && std::isdigit(static_cast<unsigned char>(c)))) {
      return false;
    }
  }
  return true;
}

std::string SqlLiteral(const Value& v) {
  if (v.IsNull()) {
    return "NULL";
  }
  switch (v.type) {
    case cella::storage::ValueType::kBool: return v.bool_val ? "true" : "false";
    case cella::storage::ValueType::kInt32:
    case cella::storage::ValueType::kInt64:
    case cella::storage::ValueType::kFloat:
    case cella::storage::ValueType::kDouble: return NumberLiteral(v);
    default: break;
  }
  std::string out = "'";
  for (const char c : v.str_val) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string KeyCondition(const RowKey& key) {
  std::string out;
  for (size_t i = 0; i < key.columns.size(); ++i) {
    if (i != 0) {
      out += " and ";
    }
    out += key.columns[i];
    const Value& v = i < key.values.size() ? key.values[i] : Value::Null();
    if (v.IsNull()) {
      out += " is null";  // 绝不生成 "= NULL"（三值逻辑下永不成立）
    } else {
      out += " = " + SqlLiteral(v);
    }
  }
  return out;
}

std::string DefaultSortColumn(const cella::db::CatalogTable& table) {
  for (const auto& c : table.columns) {
    if (c.primary_key) {
      return c.name;
    }
  }
  return "rowid";
}

namespace {

// SELECT 主体：get <列> in <表> [ordered X asc|desc] [limit <cond>] [page N, M]
std::string SelectSql(const std::string& table, const std::vector<std::string>& columns,
                      const std::string& order_col, bool desc, const std::string& where,
                      int page, int page_size) {
  std::string out = "get ";
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += columns[i];
  }
  out += " in " + table;
  if (!where.empty()) {
    out += " limit " + where;
  }
  if (!order_col.empty()) {
    out += " ordered " + order_col + (desc ? " desc" : " asc");
  }
  if (page > 0 && page_size > 0) {
    out += " page " + std::to_string(page) + ", " + std::to_string(page_size);
  }
  out += ";";
  return out;
}

}  // namespace

std::string SelectPageSql(const std::string& table, const std::vector<std::string>& columns,
                          const std::string& order_col, bool desc, int page, int page_size) {
  return SelectSql(table, columns, order_col, desc, std::string(), page, page_size);
}

std::string SelectByKeySql(const std::string& table, const std::vector<std::string>& columns,
                           const RowKey& key) {
  return SelectSql(table, columns, std::string(), false, KeyCondition(key), 0, 0);
}

std::string CountSql(const std::string& table, const std::string& first_col) {
  // 方言没有聚合函数，总行数只能取一列全量后计数（见 PLAN §6.2）
  return "get " + first_col + " in " + table + ";";
}

std::string InsertSql(const std::string& table, const std::vector<std::string>& cols,
                      const std::vector<Value>& vals) {
  std::string out = "INSERT INTO " + table + "(";
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += cols[i];
  }
  out += ") VALUES (";
  for (size_t i = 0; i < vals.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += SqlLiteral(i < vals.size() ? vals[i] : Value::Null());
  }
  out += ");";
  return out;
}

std::string UpdateSql(const std::string& table, const std::vector<std::string>& set_cols,
                      const std::vector<Value>& set_vals, const RowKey& key) {
  std::string out = "UPDATE " + table + " SET ";
  for (size_t i = 0; i < set_cols.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += set_cols[i] + " = " + SqlLiteral(i < set_vals.size() ? set_vals[i] : Value::Null());
  }
  out += " limit " + KeyCondition(key) + ";";
  return out;
}

std::string DeleteSql(const std::string& table, const RowKey& key) {
  return "DELETE in " + table + " limit " + KeyCondition(key) + ";";
}

bool CellFromJson(const JsonValue& v, const std::string& declared_type, Value* out,
                  std::string* err) {
  const std::string t = Upper(declared_type);
  if (v.IsNull()) {
    *out = Value::Null();
    return true;
  }
  if (t == "INT64") {  // rowid 伪列专用（不在目录类型里）
    if (!v.IsInt()) {
      if (err != nullptr) {
        *err = "rowid 需要整数";
      }
      return false;
    }
    *out = Value::BigInt(v.AsInt());
    return true;
  }
  if (IsNumericDeclared(t)) {
    if (!v.IsNumber()) {
      if (err != nullptr) {
        *err = "列类型 " + t + " 需要数值";
      }
      return false;
    }
    if (t == "INT" || t == "INTEGER") {
      // JSON 整数通道优先；带小数的数值即使整值也拒绝（与引擎的整数语义一致由前端把关）
      if (!v.IsInt()) {
        if (err != nullptr) {
          *err = "列类型 " + t + " 需要整数";
        }
        return false;
      }
      const std::int64_t x = v.AsInt();
      if (x < -2147483648LL || x > 2147483647LL) {
        if (err != nullptr) {
          *err = "整数超出 INT 范围";
        }
        return false;
      }
      *out = Value::Int(static_cast<std::int32_t>(x));
      return true;
    }
    if (t == "FLOAT") {
      *out = Value::Float(static_cast<float>(v.AsDouble()));
      return true;
    }
    *out = Value::Double(v.AsDouble());
    return true;
  }
  if (IsTextDeclared(t)) {
    if (!v.IsString()) {
      if (err != nullptr) {
        *err = "列类型 " + t + " 需要字符串";
      }
      return false;
    }
    *out = Value::Varchar(v.AsString());
    return true;
  }
  if (err != nullptr) {
    *err = "未知列类型: " + declared_type;
  }
  return false;
}

bool PrimaryKeyOf(const cella::db::CatalogTable& table, RowKey* key) {
  std::vector<std::string> pk_cols;
  for (const auto& c : table.columns) {
    if (c.primary_key) {
      pk_cols.push_back(c.name);
    }
  }
  if (pk_cols.empty()) {
    if (key != nullptr) {
      *key = RowKey{};
    }
    return false;
  }
  if (key != nullptr) {
    key->kind = RowKey::Kind::kPrimary;
    key->columns = pk_cols;
    key->values.assign(pk_cols.size(), Value::Null());  // 值由调用方按行填
  }
  return true;
}

RowKey RowidKey(std::int64_t rowid) {
  RowKey key;
  key.kind = RowKey::Kind::kRowid;
  key.columns.push_back("rowid");
  key.values.push_back(Value::BigInt(rowid));
  return key;
}

}  // namespace cella::client
