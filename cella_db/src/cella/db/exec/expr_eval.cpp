#include "cella/db/exec/expr_eval.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "cella/cella_printer.h"
#include "cella/db/common/value_bridge.h"
#include "cella/storage/common/types.h"

namespace cella::db {
namespace {

bool IsNumeric(ValueType t) {
  return t == ValueType::kInt32 || t == ValueType::kInt64 || t == ValueType::kFloat ||
         t == ValueType::kDouble;
}

bool IsTextual(ValueType t) {
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

bool IsIntegralValue(const Value& v) {
  return v.type == ValueType::kInt32 || v.type == ValueType::kInt64;
}

// ── SQL LIKE 通配匹配 ──────────────────────────────────────────
//   '%' 匹配任意长度（含空）子串；'_' 匹配任意单个字符；其余字符按字面比较。
// 本系统不提供 ESCAPE 子句，因此 '%'/'_' 无法转义为字面量（已记入已知边界）。
// 算法：单星号回溯（迭代版），最坏 O(|s|·|p|)，无递归深度风险。
bool LikeMatch(const std::string& s, const std::string& p) {
  size_t si = 0, pi = 0;
  size_t star = std::string::npos;  // 最近一个 '%' 在模式中的位置
  size_t star_s = 0;                // 该 '%' 当前已吞掉的字符串位置
  while (si < s.size()) {
    if (pi < p.size() && (p[pi] == '_' || p[pi] == s[si])) {
      ++si;
      ++pi;
    } else if (pi < p.size() && p[pi] == '%') {
      star = pi++;
      star_s = si;
    } else if (star != std::string::npos) {
      pi = star + 1;  // 回退到上一个 '%' 之后，让它多吞一个字符
      si = ++star_s;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '%') {
    ++pi;  // 尾部剩余的 '%' 匹配空串
  }
  return pi == p.size();
}

// 表达式里的数值字面量在语义阶段已确认可比较（SEM-309/310），
// 这里只做运行期兜底：两侧族不同即报错，避免静默给出错误结果。
// ── 标量函数支持（与 cella_common.h::cella_findFunc 的规格表一一对应）────
std::string ScalarToText(const storage::Value& v) {
  switch (v.type) {
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate:  return v.str_val;
    case ValueType::kBool:  return v.bool_val ? "TRUE" : "FALSE";
    case ValueType::kInt32: return std::to_string(v.int32_val);
    case ValueType::kInt64: return std::to_string(v.int64_val);
    case ValueType::kFloat:  return std::to_string(v.float_val);
    case ValueType::kDouble: return std::to_string(v.double_val);
    default:                 return std::string();
  }
}

// 日期以 YYYY-MM-DD 文本存储，日期运算统一换算成「距 1970-01-01 的天数」再做整数加减。
bool ParseDate(const std::string& s, int* y, int* m, int* d) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  *y = std::stoi(s.substr(0, 4));
  *m = std::stoi(s.substr(5, 2));
  *d = std::stoi(s.substr(8, 2));
  return true;
}

int DateToSerial(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = static_cast<unsigned>(m + (m > 2 ? -3 : 9));
  const unsigned doy = (153u * mp + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

void SerialToDate(int z, int* y, int* m, int* d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int yy = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
  const unsigned mm = mp + (mp < 10 ? 3 : -9);
  *y = yy + (mm <= 2 ? 1 : 0);
  *m = static_cast<int>(mm);
  *d = static_cast<int>(dd);
}

std::string FormatDate(int y, int m, int d) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return std::string(buf);
}

std::string TrimLeft(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}
std::string TrimRight(const std::string& s) {
  size_t n = s.size();
  while (n > 0 && std::isspace(static_cast<unsigned char>(s[n - 1]))) --n;
  return s.substr(0, n);
}

DbStatus EvalScalarFunc(const std::string& fn, const std::vector<storage::Value>& a,
                        storage::Value* out) {
  const std::string text0 = a.empty() ? std::string() : ScalarToText(a[0]);
  // 字符串族
  if (fn == "UPPER") {
    std::string r = text0;
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    *out = Value::Varchar(r);
    return DbStatus::Ok();
  }
  if (fn == "LOWER") {
    std::string r = text0;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    *out = Value::Varchar(r);
    return DbStatus::Ok();
  }
  if (fn == "LENGTH" || fn == "CHAR_LENGTH") {
    *out = Value::BigInt(static_cast<int64_t>(text0.size()));
    return DbStatus::Ok();
  }
  if (fn == "TRIM") {
    *out = Value::Varchar(TrimRight(TrimLeft(text0)));
    return DbStatus::Ok();
  }
  if (fn == "LTRIM") {
    *out = Value::Varchar(TrimLeft(text0));
    return DbStatus::Ok();
  }
  if (fn == "RTRIM") {
    *out = Value::Varchar(TrimRight(text0));
    return DbStatus::Ok();
  }
  if (fn == "SUBSTR" || fn == "SUBSTRING") {
    // 下标从 1 起；起点越界返回空串；长度缺省表示取到末尾
    const int64_t start = static_cast<int64_t>(AsDouble(a[1]));
    int64_t len = (a.size() >= 3) ? static_cast<int64_t>(AsDouble(a[2]))
                                  : static_cast<int64_t>(text0.size());
    if (start < 1 || len <= 0 || static_cast<size_t>(start) > text0.size()) {
      *out = Value::Varchar(std::string());
      return DbStatus::Ok();
    }
    *out = Value::Varchar(text0.substr(static_cast<size_t>(start - 1), static_cast<size_t>(len)));
    return DbStatus::Ok();
  }
  if (fn == "REPLACE") {
    const std::string from = ScalarToText(a[1]);
    const std::string to = ScalarToText(a[2]);
    std::string r;
    if (from.empty()) {
      r = text0;
    } else {
      size_t pos = 0;
      for (;;) {
        const size_t hit = text0.find(from, pos);
        if (hit == std::string::npos) {
          r += text0.substr(pos);
          break;
        }
        r += text0.substr(pos, hit - pos) + to;
        pos = hit + from.size();
      }
    }
    *out = Value::Varchar(r);
    return DbStatus::Ok();
  }
  if (fn == "CONCAT") {
    std::string r;
    for (const auto& v : a) r += ScalarToText(v);
    *out = Value::Varchar(r);
    return DbStatus::Ok();
  }
  // 数值族
  if (fn == "ABS") {
    const double x = AsDouble(a[0]);
    *out = (a[0].type == ValueType::kInt32 || a[0].type == ValueType::kInt64)
               ? Value::BigInt(static_cast<int64_t>(x < 0 ? -x : x))
               : Value::Double(x < 0 ? -x : x);
    return DbStatus::Ok();
  }
  if (fn == "ROUND") {
    const double x = AsDouble(a[0]);
    const int n = (a.size() >= 2) ? static_cast<int>(AsDouble(a[1])) : 0;
    const double p = std::pow(10.0, n);
    *out = Value::Double(std::round(x * p) / p);
    return DbStatus::Ok();
  }
  if (fn == "CEIL") {
    *out = Value::BigInt(static_cast<int64_t>(std::ceil(AsDouble(a[0]))));
    return DbStatus::Ok();
  }
  if (fn == "FLOOR") {
    *out = Value::BigInt(static_cast<int64_t>(std::floor(AsDouble(a[0]))));
    return DbStatus::Ok();
  }
  // 日期族
  if (fn == "YEAR" || fn == "MONTH" || fn == "DAY") {
    int y = 0, m = 0, d = 0;
    if (!ParseDate(text0, &y, &m, &d)) {
      return DbStatus::Error(DbCode::kTypeMismatch, fn + " 需要 YYYY-MM-DD 格式的日期，实际为 " + text0);
    }
    *out = Value::BigInt(fn == "YEAR" ? y : (fn == "MONTH" ? m : d));
    return DbStatus::Ok();
  }
  if (fn == "DATEDIFF") {
    int y1 = 0, m1 = 0, d1 = 0, y2 = 0, m2 = 0, d2 = 0;
    if (!ParseDate(ScalarToText(a[0]), &y1, &m1, &d1) ||
        !ParseDate(ScalarToText(a[1]), &y2, &m2, &d2)) {
      return DbStatus::Error(DbCode::kTypeMismatch, "DATEDIFF 需要两个 YYYY-MM-DD 格式的日期");
    }
    *out = Value::BigInt(static_cast<int64_t>(DateToSerial(y1, m1, d1) - DateToSerial(y2, m2, d2)));
    return DbStatus::Ok();
  }
  if (fn == "DATE_ADD" || fn == "DATE_SUB") {
    int y = 0, m = 0, d = 0;
    if (!ParseDate(text0, &y, &m, &d)) {
      return DbStatus::Error(DbCode::kTypeMismatch, fn + " 需要 YYYY-MM-DD 格式的日期，实际为 " + text0);
    }
    const int64_t delta = static_cast<int64_t>(AsDouble(a[1])) * (fn == "DATE_SUB" ? -1 : 1);
    int ry = 0, rm = 0, rd = 0;
    SerialToDate(DateToSerial(y, m, d) + static_cast<int>(delta), &ry, &rm, &rd);
    *out = Value::Varchar(FormatDate(ry, rm, rd));
    return DbStatus::Ok();
  }
  return DbStatus::Error(DbCode::kInternal, "未知标量函数: " + fn);
}

DbStatus CheckComparable(const Value& a, const Value& b) {
  const bool an = IsNumeric(a.type);
  const bool bn = IsNumeric(b.type);
  if (an && bn) {
    return DbStatus::Ok();
  }
  const bool at = IsTextual(a.type);
  const bool bt = IsTextual(b.type);
  if (at && bt) {
    return DbStatus::Ok();
  }
  if (a.type == ValueType::kBool && b.type == ValueType::kBool) {
    return DbStatus::Ok();
  }
  return DbStatus::Error(DbCode::kTypeMismatch,
                         std::string("无法比较 ") + storage::ToString(a.type) + " 与 " +
                             storage::ToString(b.type));
}

// 标量相等比较（供 IN 列表使用）：先按族的相容性把关，再逐族比较
DbStatus ScalarEq(const storage::Value& a, const storage::Value& b, bool* eq) {
  const DbStatus s = CheckComparable(a, b);
  if (!s.ok()) {
    return s;
  }
  if (IsNumeric(a.type) && IsNumeric(b.type)) {
    *eq = (AsDouble(a) == AsDouble(b));
    return DbStatus::Ok();
  }
  if (a.type == ValueType::kBool && b.type == ValueType::kBool) {
    *eq = (a.bool_val == b.bool_val);
    return DbStatus::Ok();
  }
  *eq = (ScalarToText(a) == ScalarToText(b));
  return DbStatus::Ok();
}

DbStatus ArithNumeric(const Value& a, const Value& b, const char* op, storage::Value* out) {
  if (!IsNumeric(a.type) || !IsNumeric(b.type)) {
    return DbStatus::Error(DbCode::kTypeMismatch,
                           std::string("运算符 ") + op + " 需要数值操作数");
  }
  if (IsIntegralValue(a) && IsIntegralValue(b)) {
    const int64_t x = (a.type == ValueType::kInt32) ? a.int32_val : a.int64_val;
    const int64_t y = (b.type == ValueType::kInt32) ? b.int32_val : b.int64_val;
    int64_t r = 0;
    bool ok_int = true;
    const char c = op[0];
    if (c == '+') {
      r = x + y;
    } else if (c == '-') {
      r = x - y;
    } else if (c == '*') {
      r = x * y;
    } else {  // '/'
      if (y == 0) {
        return DbStatus::Error(DbCode::kDivisionByZero, "除数为 0");
      }
      if (x % y == 0) {
        r = x / y;
      } else {
        ok_int = false;  // 不整除 → 退化为浮点结果
      }
    }
    if (ok_int && r >= std::numeric_limits<int32_t>::min() &&
        r <= std::numeric_limits<int32_t>::max()) {
      *out = Value::Int(static_cast<int32_t>(r));
      return DbStatus::Ok();
    }
  }
  // 浮点路径
  const double x = AsDouble(a);
  const double y = AsDouble(b);
  const char c = op[0];
  if (c == '/') {
    if (y == 0.0) {
      return DbStatus::Error(DbCode::kDivisionByZero, "除数为 0");
    }
    *out = Value::Double(x / y);
    return DbStatus::Ok();
  }
  *out = Value::Double((c == '+') ? (x + y) : ((c == '-') ? (x - y) : (x * y)));
  return DbStatus::Ok();
}

}  // namespace

DbStatus ExprEval::Eval(const cella::CELLA_Expr& expr, const EvalRow& row, storage::Value* out) {
  if (out == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "Eval: out 为空");
  }
  switch (expr.kind) {
    case cella::CELLA_Expr::Kind::LITERAL:
      *out = LiteralToValue(expr);
      return DbStatus::Ok();

    case cella::CELLA_Expr::Kind::COLUMN_REF: {
      if (row.fields == nullptr || row.values == nullptr) {
        return DbStatus::Error(DbCode::kInternal, "列引用求值缺少行上下文");
      }
      // 解析规则与 RowSet::Resolve 一致：限定引用比 qualifier，裸引用只比列名
      int found = -1;
      const std::string want_name = cella::cella_toUpper(expr.column);
      const std::string want_qual = cella::cella_toUpper(expr.table);
      for (size_t i = 0; i < row.fields->size(); ++i) {
        if (cella::cella_toUpper((*row.fields)[i].name) != want_name) {
          continue;
        }
        if (!want_qual.empty() && cella::cella_toUpper((*row.fields)[i].qualifier) != want_qual) {
          continue;
        }
        found = static_cast<int>(i);
        break;
      }
      if (found < 0) {
        const std::string shown = expr.table.empty() ? expr.column : expr.table + "." + expr.column;
        return DbStatus::Error(DbCode::kColumnNotFound, "列不存在: " + shown);
      }
      if (static_cast<size_t>(found) >= row.values->size()) {
        return DbStatus::Error(DbCode::kInternal, "行值与字段数不一致");
      }
      *out = (*row.values)[static_cast<size_t>(found)];
      return DbStatus::Ok();
    }

    case cella::CELLA_Expr::Kind::AGGREGATE: {
      // 聚合函数在 OpAggregate 里已经算完，并作为一列出现在输入行中；
      // 这里只需按「与 OpAggregate 完全一致的列名」取回该列的值。
      if (row.fields == nullptr || row.values == nullptr) {
        return DbStatus::Error(DbCode::kInternal, "聚合求值缺少行上下文");
      }
      const std::string fn = expr.aggFunc.empty() ? "COUNT" : expr.aggFunc;
      const std::string want =
          expr.aggStar
              ? fn + "(*)"
              : fn + "(" + (expr.table.empty() ? expr.column : expr.table + "." + expr.column) + ")";
      const std::string want_up = cella::cella_toUpper(want);
      for (size_t i = 0; i < row.fields->size(); ++i) {
        if (cella::cella_toUpper((*row.fields)[i].name) == want_up) {
          if (i >= row.values->size()) {
            return DbStatus::Error(DbCode::kInternal, "行值与字段数不一致");
          }
          *out = (*row.values)[i];
          return DbStatus::Ok();
        }
      }
      return DbStatus::Error(DbCode::kColumnNotFound,
                             "聚合结果列不存在: " + want + "（OpAggregate 未产出该列）");
    }

    case cella::CELLA_Expr::Kind::FUNCTION: {
      std::vector<storage::Value> args;
      args.reserve(expr.args.size());
      for (const auto& arg : expr.args) {
        storage::Value v;
        const DbStatus s = Eval(*arg, row, &v);
        if (!s.ok()) {
          return s;
        }
        // 除 CONCAT（约定 NULL 视作空串）外一律 NULL 传播
        if (v.IsNull() && expr.funcName != "CONCAT") {
          *out = Value::Null();
          return DbStatus::Ok();
        }
        args.push_back(v);
      }
      return EvalScalarFunc(expr.funcName, args, out);
    }

    case cella::CELLA_Expr::Kind::IN_LIST: {
      if (!expr.left) {
        return DbStatus::Error(DbCode::kInternal, "IN 表达式缺少左操作数");
      }
      storage::Value lv;
      const DbStatus s = Eval(*expr.left, row, &lv);
      if (!s.ok()) {
        return s;
      }
      if (lv.IsNull()) {
        *out = Value::Null();  // NULL IN (...) → UNKNOWN
        return DbStatus::Ok();
      }
      bool hit = false;
      bool any_null = false;
      for (const auto& e : expr.inList) {
        storage::Value rv;
        const DbStatus rs = Eval(*e, row, &rv);
        if (!rs.ok()) {
          return rs;
        }
        if (rv.IsNull()) {
          any_null = true;
          continue;
        }
        bool eq = false;
        const DbStatus es = ScalarEq(lv, rv, &eq);
        if (!es.ok()) {
          return es;
        }
        if (eq) {
          hit = true;
          break;
        }
      }
      if (hit) {
        *out = Value::Bool(!expr.negated);
      } else if (any_null) {
        *out = Value::Null();  // 未命中但存在 NULL → UNKNOWN
      } else {
        *out = Value::Bool(expr.negated);
      }
      return DbStatus::Ok();
    }

    // 以下三类需要「在表达式求值过程中递归执行一条完整查询」，
    // 而 ExprEval 目前不持有执行器句柄。给出明确错误而不是静默返回错值。
    case cella::CELLA_Expr::Kind::IN_QUERY:
      return DbStatus::Error(DbCode::kNotImplemented, "执行期尚未支持子查询 IN (GET ...)");
    case cella::CELLA_Expr::Kind::EXISTS_Q:
      return DbStatus::Error(DbCode::kNotImplemented, "执行期尚未支持 EXISTS (GET ...)");
    case cella::CELLA_Expr::Kind::SCALAR_Q:
      return DbStatus::Error(DbCode::kNotImplemented, "执行期尚未支持标量子查询 (GET ...)");
    case cella::CELLA_Expr::Kind::WINDOW:
      return DbStatus::Error(DbCode::kNotImplemented, "执行期尚未支持窗口函数 OVER (...)");

    case cella::CELLA_Expr::Kind::UNARY: {
      if (!expr.child) {
        return DbStatus::Error(DbCode::kInternal, "一元表达式缺少子节点");
      }
      // 判空：结果恒为 TRUE/FALSE（不走三值比较），这是筛 NULL 行的唯一合法写法
      if (expr.uop == cella::CELLA_Expr::UnOp::IS_NULL ||
          expr.uop == cella::CELLA_Expr::UnOp::IS_NOT_NULL) {
        storage::Value inner;
        const DbStatus s = Eval(*expr.child, row, &inner);
        if (!s.ok()) {
          return s;
        }
        const bool is_null = inner.IsNull();
        *out = Value::Bool(expr.uop == cella::CELLA_Expr::UnOp::IS_NULL ? is_null : !is_null);
        return DbStatus::Ok();
      }
      if (expr.uop == cella::CELLA_Expr::UnOp::NOT) {
        bool v = false;
        bool is_null = false;
        const DbStatus s = EvalBool(*expr.child, row, &v, &is_null);
        if (!s.ok()) {
          return s;
        }
        *out = is_null ? Value::Null() : Value::Bool(!v);
        return DbStatus::Ok();
      }
      storage::Value inner;
      const DbStatus s = Eval(*expr.child, row, &inner);
      if (!s.ok()) {
        return s;
      }
      if (inner.IsNull()) {
        *out = Value::Null();
        return DbStatus::Ok();
      }
      if (!IsNumeric(inner.type)) {
        return DbStatus::Error(DbCode::kTypeMismatch, "一元负号需要数值操作数");
      }
      switch (inner.type) {
        case ValueType::kInt32:
          *out = Value::Int(-inner.int32_val);
          break;
        case ValueType::kInt64:
          *out = Value::BigInt(-inner.int64_val);
          break;
        case ValueType::kFloat:
          *out = Value::Float(-inner.float_val);
          break;
        default:
          *out = Value::Double(-inner.double_val);
          break;
      }
      return DbStatus::Ok();
    }

    case cella::CELLA_Expr::Kind::BINARY:
      break;
  }

  // ── 二元运算 ──
  if (!expr.left || !expr.right) {
    return DbStatus::Error(DbCode::kInternal, "二元表达式缺少子节点");
  }

  // 布尔连接词走三值逻辑
  if (expr.bop == cella::CELLA_Expr::BinOp::AND || expr.bop == cella::CELLA_Expr::BinOp::OR) {
    bool l = false, r = false;
    bool ln = false, rn = false;
    const DbStatus sl = EvalBool(*expr.left, row, &l, &ln);
    if (!sl.ok()) {
      return sl;
    }
    const DbStatus sr = EvalBool(*expr.right, row, &r, &rn);
    if (!sr.ok()) {
      return sr;
    }
    const bool is_and = (expr.bop == cella::CELLA_Expr::BinOp::AND);
    if (is_and) {
      if ((!ln && !l) || (!rn && !r)) {         // 任一侧确定 FALSE
        *out = Value::Bool(false);
      } else if (ln || rn) {                     // 无 FALSE 但有 UNKNOWN
        *out = Value::Null();
      } else {
        *out = Value::Bool(true);
      }
    } else {
      if ((!ln && l) || (!rn && r)) {            // 任一侧确定 TRUE
        *out = Value::Bool(true);
      } else if (ln || rn) {
        *out = Value::Null();
      } else {
        *out = Value::Bool(false);
      }
    }
    return DbStatus::Ok();
  }

  storage::Value a;
  storage::Value b;
  const DbStatus sa = Eval(*expr.left, row, &a);
  if (!sa.ok()) {
    return sa;
  }
  const DbStatus sb = Eval(*expr.right, row, &b);
  if (!sb.ok()) {
    return sb;
  }

  switch (expr.bop) {
    case cella::CELLA_Expr::BinOp::PLUS:
    case cella::CELLA_Expr::BinOp::MINUS:
    case cella::CELLA_Expr::BinOp::MUL:
    case cella::CELLA_Expr::BinOp::DIV: {
      if (a.IsNull() || b.IsNull()) {
        *out = Value::Null();
        return DbStatus::Ok();
      }
      const char* op = (expr.bop == cella::CELLA_Expr::BinOp::PLUS)    ? "+"
                       : (expr.bop == cella::CELLA_Expr::BinOp::MINUS) ? "-"
                       : (expr.bop == cella::CELLA_Expr::BinOp::MUL)   ? "*"
                                                                      : "/";
      return ArithNumeric(a, b, op, out);
    }
    default:
      break;
  }

  // 通配匹配在比较运算之前分流：它不参与「可比性」检查，规则是纯文本的。
  if (expr.bop == cella::CELLA_Expr::BinOp::LIKE ||
      expr.bop == cella::CELLA_Expr::BinOp::NOT_LIKE) {
    if (a.IsNull() || b.IsNull()) {
      *out = Value::Null();  // UNKNOWN：NULL 不匹配任何模式
      return DbStatus::Ok();
    }
    if (!IsTextual(a.type) || !IsTextual(b.type)) {
      return DbStatus::Error(DbCode::kTypeMismatch,
                             std::string("运算符 ") +
                                 (expr.bop == cella::CELLA_Expr::BinOp::LIKE ? "LIKE" : "NOT LIKE") +
                                 " 需要文本操作数");
    }
    const bool matched = LikeMatch(a.str_val, b.str_val);
    *out = Value::Bool(expr.bop == cella::CELLA_Expr::BinOp::LIKE ? matched : !matched);
    return DbStatus::Ok();
  }

  // 比较运算
  if (a.IsNull() || b.IsNull()) {
    *out = Value::Null();  // UNKNOWN
    return DbStatus::Ok();
  }
  const DbStatus cmp_ok = CheckComparable(a, b);
  if (!cmp_ok.ok()) {
    return cmp_ok;
  }
  const int c = CompareValues(a, b, nullptr);
  bool result = false;
  switch (expr.bop) {
    case cella::CELLA_Expr::BinOp::EQ: result = (c == 0); break;
    case cella::CELLA_Expr::BinOp::NE: result = (c != 0); break;
    case cella::CELLA_Expr::BinOp::LT: result = (c < 0); break;
    case cella::CELLA_Expr::BinOp::LE: result = (c <= 0); break;
    case cella::CELLA_Expr::BinOp::GT: result = (c > 0); break;
    case cella::CELLA_Expr::BinOp::GE: result = (c >= 0); break;
    default:
      return DbStatus::Error(DbCode::kInternal, "未知比较运算符");
  }
  *out = Value::Bool(result);
  return DbStatus::Ok();
}

DbStatus ExprEval::EvalBool(const cella::CELLA_Expr& expr, const EvalRow& row, bool* out,
                            bool* is_null) {
  if (out == nullptr || is_null == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "EvalBool: 输出参数为空");
  }
  storage::Value v;
  const DbStatus s = Eval(expr, row, &v);
  if (!s.ok()) {
    return s;
  }
  if (v.IsNull()) {
    *is_null = true;
    *out = false;
    return DbStatus::Ok();
  }
  if (v.type != ValueType::kBool) {
    return DbStatus::Error(DbCode::kTypeMismatch,
                           "布尔上下文需要 BOOL，实际为 " + std::string(storage::ToString(v.type)));
  }
  *is_null = false;
  *out = v.bool_val;
  return DbStatus::Ok();
}

DbStatus ExprEval::EvalPredicate(const cella::CELLA_Expr& expr, const EvalRow& row, bool* pass) {
  if (pass == nullptr) {
    return DbStatus::Error(DbCode::kInternal, "EvalPredicate: pass 为空");
  }
  bool v = false;
  bool is_null = false;
  const DbStatus s = EvalBool(expr, row, &v, &is_null);
  if (!s.ok()) {
    return s;
  }
  *pass = (!is_null && v);  // UNKNOWN 与 FALSE 同样不通过
  return DbStatus::Ok();
}

std::string ExprEval::OutputName(const cella::CELLA_Expr& expr, const std::string& alias) {
  if (!alias.empty()) {
    return alias;
  }
  // 裸列引用直接用列名（"s.name" 作为表头不友好，也与 SQL 习惯不符）；
  // 带限定符的引用（"s.name"）在排序/分组解析时会退化按列名匹配，见 Executor。
  if (expr.kind == cella::CELLA_Expr::Kind::COLUMN_REF) {
    return expr.column;
  }
  // 聚合列：与 OpAggregate 产出的列名保持一致，否则上层取不回该列
  if (expr.kind == cella::CELLA_Expr::Kind::AGGREGATE) {
    const std::string fn = expr.aggFunc.empty() ? "COUNT" : expr.aggFunc;
    if (expr.aggStar) {
      return fn + "(*)";
    }
    return fn + "(" + (expr.table.empty() ? expr.column : expr.table + "." + expr.column) + ")";
  }
  return cella::cella_exprToString(expr);
}

}  // namespace cella::db
