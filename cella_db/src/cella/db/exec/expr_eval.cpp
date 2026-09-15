#include "cella/db/exec/expr_eval.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

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
