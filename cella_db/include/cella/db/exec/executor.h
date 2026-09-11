// executor.h —— 计划执行器：遍历编译器产出的计划树，调度算子完成数据存取。
//
// 在整体系统中的位置：
//
//   SQL 文本 →[编译器]→ AST → 计划树（可执行 IR）→[本模块]→ 存储层 IStorage
//                                                        ↓
//                                                  结果集 QueryResult
//
// 它消费的是计划节点（而不是原始 AST）：算子种类、子节点关系、谓词表达式、
// 排序键、投影表达式、行数上限……都来自 CELLA_PlanNode 的「执行期附属信息」。
// 只有 DDL/DML 需要列定义/插入值/赋值这类全量类型化信息，此时通过节点上的
// stmt 回指取 AST（计划树里放不下，也不该重复）。
//
// 执行约定：
//   * 每个语句的执行都在一个事务上下文（ExecContext）内进行；锁在扫描/写表时
//     按需申请，由事务管理器在提交/回滚时统一释放（严格 2PL）。
//   * 存储层声明为非线程安全，故所有 IStorage 调用都在同一把递归互斥量内进行。
//     加锁顺序固定为「先数据库锁、后存储互斥量」，两条路径不会交叉等待。
//   * 计划树内的 stmt/tableRef 指针指向调用方持有的 AST，调用期间必须存活。
#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cella/cella_planner.h"
#include "cella/db/catalog/catalog_manager.h"
#include "cella/db/exec/query_result.h"
#include "cella/db/exec/row_set.h"
#include "cella/db/txn/transaction.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/record.h"
#include "cella/storage/common/types.h"

namespace cella::db {

// 语句执行的上下文：事务 + 触碰表记录
struct ExecContext {
  txn_id_t txn_id = kInvalidTxnId;
  Transaction* txn = nullptr;  // 可为空（只读探测场景），此时不记录 undo
  // 锁资源名的库前缀（当前库名）：锁资源记作 "<db>.<table>"，
  // 避免引擎切换数据库后两个库的同名表共享同一把锁（假冲突/假死锁）。
  std::string lock_scope;

  bool recording() const { return txn != nullptr; }
};

class Executor {
 public:
  Executor(storage::IStorage* storage, CatalogManager* catalog, TxnManager* txn_manager,
           LockManager* locks, std::recursive_mutex* storage_mutex);

  // 执行一条语句的计划；out 必须非空。
  DbStatus Execute(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);

  // 只跑查询算子子树（GET 的各类算子也可单独驱动，便于测试）
  DbStatus Run(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);

  // 统计：执行的算子次数（可观测「计划驱动」确实发生了）
  size_t operator_calls() const { return operator_calls_; }
  void ResetOperatorCalls() { operator_calls_ = 0; }

 private:
  // ── 语句级算子 ──
  DbStatus ExecCreateTable(const cella::CELLA_PlanNode& plan, const ExecContext& ctx,
                           QueryResult* out);
  DbStatus ExecDropTable(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecInsert(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecDelete(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecUpdate(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);
  DbStatus ExecGet(const cella::CELLA_PlanNode& plan, const ExecContext& ctx, QueryResult* out);

  // ── 查询算子（与计划节点一一对应）──
  DbStatus OpSeqScan(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpFilter(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpProject(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpDistinct(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpAggregate(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpSort(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpLimit(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpPage(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpJoin(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);
  DbStatus OpUnion(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);

  // 只允许一个子节点的算子的统一入口
  DbStatus RunSingleChild(const cella::CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out);

  // ── 工具 ──
  DbStatus LockTable(const std::string& table, LockMode mode, const ExecContext& ctx);
  // 扫描一张表并按谓词过滤，收集 (Rid, 记录)（DELETE/UPDATE 两阶段修改用）
  DbStatus ScanMatching(const CatalogTable& table, const cella::CELLA_Expr* pred,
                        std::vector<std::pair<storage::Rid, storage::Record>>* out);
  // 解析 SeqScan 的 detail 文本 "[name alias]"（tableRef 缺失时的兜底）
  static bool ParseTableDisplay(const std::string& detail, std::string* name, std::string* alias);

  storage::IStorage* storage_;
  CatalogManager* catalog_;
  TxnManager* txn_manager_;
  LockManager* locks_;
  std::recursive_mutex* storage_mutex_;
  size_t operator_calls_ = 0;

  // ── ORDER BY 引用未投影列时的「隐藏排序键」通道 ──
  // 计划形状固定为 Project → Distinct → Sort，而 SQL 语义里 ORDER BY 作用在
  // 投影之前（get name in student ordered age desc 是合法的）。为了不改动计划文本
  // （golden 契约），执行期这样做：
  //   OpSort 先把需要的排序键放进 sort_hint_；
  //   下方的 OpProject 在正常输出列之后追加同序的「隐藏列」（键表达式）；
  //   OpDistinct 只按可见前缀去重（隐藏列不参与 DISTINCT 语义）；
  //   OpSort 排序后把隐藏列裁掉，输出与 SQL 语义一致。
  const std::vector<cella::CELLA_ColName>* sort_hint_ = nullptr;
  size_t visible_fields_ = 0;  // Project 输出的可见列数；0 表示没有隐藏列
};

}  // namespace cella::db
