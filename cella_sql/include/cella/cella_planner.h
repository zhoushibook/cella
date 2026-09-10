// cella_planner.h —— 执行计划生成接口：通过语义检查的 AST -> 执行计划树（任务书 3.4 节）。
// 输出为缩进树文本的节点结构；计划阶段错误记为 PLN-4xx。
//
// 【整合说明】计划树同时承担两个角色：
//   1) 文本视图：`cella_printPlan` 打印缩进树（op/detail/extra），golden 回归以此为契约；
//   2) 执行视图：`cella_db` 执行层遍历计划节点直接调度算子。为此节点上附加了
//      「执行期附属信息」（stmt/tableRef/exprs/sortKeys/...），见下方分隔线。
//   打印函数不读取执行期字段，因此文本输出与整合前逐字节一致；
//   优化器 cloneShell 会逐字段深拷贝，常量折叠后执行信息不丢失。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cella_ast.h"
#include "cella_catalog.h"
#include "cella_common.h"

namespace cella
{

    // 计划节点：op=算子名，detail=同行附加信息（可空），extra=子行信息，children=子算子
    // pred/onExpr：Filter 谓词与 Join ON 条件的表达式副本（供优化器做常量折叠/布尔化简）
    struct CELLA_PlanNode
    {
        std::string op;
        std::string detail;
        std::vector<std::string> extra;
        int line = 0, col = 0;
        std::unique_ptr<CELLA_Expr> pred;   // Filter 谓词（可空）
        std::unique_ptr<CELLA_Expr> onExpr; // Join ON 条件（可空）
        std::string joinKind;               // Join 方向：middle/left/right（Join 节点）
        std::vector<std::unique_ptr<CELLA_PlanNode>> children;

        // ─────────────── 执行期附属信息（cella_db 执行层使用）───────────────
        // 约定：以上字段描述「打印什么」，以下字段描述「怎么执行」。
        // 指针指向 CELLA_Program 内的对象，生命周期不短于计划树本身。

        // DDL/DML：语句回指。AST 保存了完整且类型化的信息（列定义、插入值、SET 赋值、
        // 过滤谓词），执行层据此完成数据变更，无需反向解析 detail/extra 文本。
        const CELLA_Stmt *stmt = nullptr;

        // SeqScan：表引用（表名 + 可选别名），决定行上下文的限定名
        const CELLA_TableRef *tableRef = nullptr;

        // Project：选择表达式与输出列名（与 exprs 等长；无别名时由执行层按表达式文本生成）
        std::vector<std::unique_ptr<CELLA_Expr>> exprs;
        std::vector<std::string> selectAliases;

        // Sort：排序键与方向（等长）
        std::vector<CELLA_ColName> sortKeys;
        std::vector<bool> sortAsc;

        // Aggregate：分组列（GROUP BY 键）。本方言未定义聚合函数（COUNT/SUM/...），
        // 执行层据此做「分组去重」——每个分组输出该组第一行。
        std::vector<CELLA_ColName> groupKeys;

        // Limit / Page：行数上限与分页偏移（-1 = 未指定）
        long long rowLimit = -1;
        long long pageOffset = -1;
        long long pageSize = -1;
    };

    // 只为 stmtOk 为真的语句生成计划；错误追加到 errors
    // insertCols: 与语句对齐，INSERT 的实际目标列（省略列清单时已由语义阶段展开为全部列）
    std::vector<std::unique_ptr<CELLA_PlanNode>> cella_plan(const CELLA_Program &program,
                                                            const std::vector<bool> &stmtOk,
                                                            const std::vector<std::vector<std::string>> &insertCols,
                                                            std::vector<CELLA_Error> &errors);

} // namespace cella
