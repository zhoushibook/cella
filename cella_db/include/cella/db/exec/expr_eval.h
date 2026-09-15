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
//
// ── 两类「本层算不出来」的表达式 ────────────────────────────────
// 子查询与窗口函数都需要跳出「一行值」的视野：
//   * 子查询要递归跑一条完整查询（本层没有执行器句柄）；
//   * 窗口函数要先看完全部分区、组内排序后才能定值。
// 两者都不改求值器结构，而是通过 EvalCtx 这个**窄接口**旁路：
//   * 子查询 → SubqueryRunner 回调，由执行器实现（编译 + 执行 + 语句级缓存）；
//   * 窗口值 → 执行器在投影前算好整列，按「表达式节点地址」喂进来。
// 这样 ExprEval 依然是无状态纯函数，测试可以直接喂一个假的 EvalCtx 驱动。
#pragma once

#include <map>
#include <string>
#include <vector>

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

// ── 子查询执行器（由 Executor 实现）──────────────────────────
//
// 只暴露「跑一条子查询，把首列物化出来」这一个动作 —— ExprEval 不需要知道
// 事务、锁、目录这些事。语义阶段已确认子查询**不与外层相关**（见
// cella_semantic.cpp::resolveSubqueryExpr 的已知边界），因此结果与外层行无关，
// 执行器可以对同一个子查询体只跑一次（语句级缓存）。
class SubqueryRunner {
 public:
  virtual ~SubqueryRunner() = default;

  // 执行子查询体 sub。
  //   exists_only = true  → 只判断「有没有行」，values 不被填充（EXISTS 用）；
  //   exists_only = false → 把首列的每一行依次填入 values（IN / 标量用）。
  // 标量子查询的「至多一行」约束由调用方按 values.size() 判定。
  virtual DbStatus RunSubquery(const cella::CELLA_Stmt& sub, bool exists_only,
                               std::vector<storage::Value>* values, bool* any_row) = 0;
};

// 求值旁路上下文。默认全空 —— 此时遇到子查询/窗口函数会给出明确错误
// （而不是静默返回错值），保证「没接执行器的调用点」不会悄悄算错。
struct EvalCtx {
  SubqueryRunner* sub = nullptr;
  // 当前行的窗口函数值：键 = 窗口表达式节点的地址（计划树持有表达式，地址稳定）。
  const std::map<const cella::CELLA_Expr*, storage::Value>* window_values = nullptr;

  bool has_subquery_runner() const { return sub != nullptr; }
};

class ExprEval {
 public:
  // 求值一个表达式。列引用无法解析时报 kColumnNotFound。
  // ctx 可为空（无子查询/窗口能力的场景）；此时遇到那两类节点返回 DB-703。
  static DbStatus Eval(const cella::CELLA_Expr& expr, const EvalRow& row, storage::Value* out,
                       EvalCtx* ctx = nullptr);

  // 求值为布尔（三值逻辑）：*is_null 为 true 表示结果是 UNKNOWN。
  static DbStatus EvalBool(const cella::CELLA_Expr& expr, const EvalRow& row, bool* out,
                           bool* is_null, EvalCtx* ctx = nullptr);

  // 谓词过滤语义：只有确定为 TRUE 才算通过
  static DbStatus EvalPredicate(const cella::CELLA_Expr& expr, const EvalRow& row, bool* pass,
                                EvalCtx* ctx = nullptr);

  // 输出列名：优先用别名，否则用表达式文本（与计划打印同一套渲染规则）
  static std::string OutputName(const cella::CELLA_Expr& expr, const std::string& alias);

  // 表达式树里是否含窗口函数节点（执行器据此决定是否走「整列预算」的投影路径）。
  static bool HasWindow(const cella::CELLA_Expr& expr);
  // 收集表达式树里的全部窗口函数节点（按出现顺序，同一节点只收一次）。
  static void CollectWindows(const cella::CELLA_Expr& expr,
                             std::vector<const cella::CELLA_Expr*>* out);
};

}  // namespace cella::db
