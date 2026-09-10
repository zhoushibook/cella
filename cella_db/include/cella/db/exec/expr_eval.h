// expr_eval.h —— 表达式求值器：在「一行值」上对编译器产出的表达式树求值。
//
// 输入是编译器的 CELLA_Expr（AST 节点，类型已在语义阶段校验过），
// 输出是存储层的 Value（可直接写入记录 / 参与比较）。中间不做隐式跨族转换：
// 语义阶段放过的 NUMBER→INT/FLOAT/DOUBLE、STRING→字符族在这里按目标列类型
// 通过 CoerceValue 落地；表达式内部运算遵循「数值族提升 + NULL 传播」。
//
// 三值逻辑（SQL 语义）：
//   与 NULL 的比较 = UNKNOWN；UNKNOWN 参与 AND/OR 按 Kleene 真值表传播。
//   WHERE/limit 只接受 TRUE，UNKNOWN 与 FALSE 一样被过滤掉。
#pragma once

#include <string>

#include "cella/cella_ast.h"
#include "cella/db/common/db_status.h"
#include "cella/db/exec/row_set.h"
#include "cella/storage/common/value.h"

namespace cella::db {

// 求值上下文：字段定义 + 一行值（两者等长）
struct EvalRow {
  const std::vector<FieldRef>* fields = nullptr;
  const std::vector<storage::Value>* values = nullptr;

  static EvalRow Empty() { return EvalRow{}; }
};

class ExprEval {
 public:
  // 求值一个表达式。列引用无法解析时报 kColumnNotFound。
  static DbStatus Eval(const cella::CELLA_Expr& expr, const EvalRow& row, storage::Value* out);

  // 求值为布尔（三值逻辑）：*is_null 为 true 表示结果是 UNKNOWN。
  static DbStatus EvalBool(const cella::CELLA_Expr& expr, const EvalRow& row, bool* out,
                           bool* is_null);

  // 谓词过滤语义：只有确定为 TRUE 才算通过
  static DbStatus EvalPredicate(const cella::CELLA_Expr& expr, const EvalRow& row, bool* pass);

  // 输出列名：优先用别名，否则用表达式文本（与计划打印同一套渲染规则）
  static std::string OutputName(const cella::CELLA_Expr& expr, const std::string& alias);
};

}  // namespace cella::db
