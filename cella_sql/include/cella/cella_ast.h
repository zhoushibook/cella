// cella_ast.h —— AST 定义：语句与表达式节点（任务书 3.2 节）。
// 采用 tagged 结构 + unique_ptr 拥有子节点；每个节点携带源位置(行:列，1 起)。
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cella_catalog.h"

namespace cella
{

    struct CELLA_Stmt; // 前向声明：子查询表达式持有语句指针

    // ---------------- 表达式 ----------------

    enum class CELLA_LiteralKind
    {
        NUMBER,
        STRING,
        DATE,
        NULL_LIT,
        BOOL_LIT,
        DEFAULT_LIT // INSERT ... VALUES ( DEFAULT )：取该列的默认值
    };

    // 列名引用（可带表限定）。必须在 CELLA_Expr 之前定义：
    // 表达式的窗口子句 WINDOW 需要按值持有它，前置声明会让 vector 成员实例化为不完整类型。
    struct CELLA_ColName
    {
        std::string table; // 可空
        std::string column;
        int line = 0, col = 0;
    };

    struct CELLA_Expr
    {
        enum class Kind
        {
            LITERAL,
            COLUMN_REF,
            UNARY,
            BINARY,
            AGGREGATE, // COUNT(*) / COUNT(col) / SUM / AVG / MIN / MAX（P4 + 收尾补齐）
            FUNCTION,  // 标量函数：UPPER(x) / LENGTH(s) / SUBSTR(s,a,n) / DATEDIFF(a,b)
            IN_LIST,   // x [NOT] IN ( v1, v2, ... )      —— left = 被比较表达式
            IN_QUERY,  // x [NOT] IN ( get ... )          —— left + subquery
            EXISTS_Q,  // [NOT] EXISTS ( get ... )
            SCALAR_Q,  // ( get ... ) 标量子查询（至多一行一列）
            WINDOW     // fn ( args ) OVER ( PARTITION ... ORDERED ... )
        };
        enum class UnOp
        {
            NEG,
            NOT,
            IS_NULL,      // 后缀判空：x IS NULL（结果恒 TRUE/FALSE，不走三值比较）
            IS_NOT_NULL   // 后缀判空：x IS NOT NULL
        };
        enum class BinOp
        {
            EQ,
            NE,
            LT,
            LE,
            GT,
            GE,
            PLUS,
            MINUS,
            MUL,
            DIV,
            AND,
            OR,
            LIKE,     // 字符串通配比较（二元比较符，与 = / < 同级）
            NOT_LIKE  // x NOT LIKE 'p'（解析期由 NOT + LIKE 合成一个二元符）
        };

        Kind kind = Kind::LITERAL;
        int line = 0, col = 0;

        // LITERAL
        CELLA_LiteralKind lit = CELLA_LiteralKind::NULL_LIT;
        std::string text;     // 数字规范化文本 / 字符串内容 / 日期文本
        double num = 0;       // NUMBER 数值
        bool boolVal = false; // BOOL_LIT

        // COLUMN_REF
        std::string table; // 可空；非空表示 表名.列名
        std::string column;

        // UNARY / BINARY
        UnOp uop = UnOp::NEG;
        BinOp bop = BinOp::EQ;
        std::unique_ptr<CELLA_Expr> left;  // BINARY 左子树
        std::unique_ptr<CELLA_Expr> right; // BINARY 右子树
        std::unique_ptr<CELLA_Expr> child; // UNARY 子树

        // AGGREGATE（COUNT / SUM / AVG / MIN / MAX）
        bool aggStar = false;              // COUNT(*) → true；其余函数一律 false
        std::string aggFunc;               // 函数名原文，大写（"COUNT"/"SUM"/"AVG"/"MIN"/"MAX"）
        // 复用 column/table 字段承载聚合函数的列引用

        // FUNCTION / WINDOW
        std::string funcName;                                  // 标量/窗口函数名，大写
        std::vector<std::unique_ptr<CELLA_Expr>> args;          // 函数实参（0..n）

        // IN_LIST / IN_QUERY / EXISTS_Q / SCALAR_Q
        bool negated = false;                                  // NOT IN / NOT EXISTS
        std::vector<std::unique_ptr<CELLA_Expr>> inList;        // IN_LIST 的值列表
        std::unique_ptr<CELLA_Stmt> subquery;                   // 子查询语句（GET）

        // WINDOW
        std::vector<CELLA_ColName> winPartition;               // PARTITION BY 列
        std::vector<CELLA_ColName> winOrder;                   // ORDERED BY 列
        std::vector<bool> winOrderAsc;                          // 与 winOrder 等长
    };

    // ---------------- 结构 ----------------

    struct CELLA_ColumnDef
    {
        std::string name;
        CELLA_DataType type = CELLA_DataType::INT;
        int len = 0;
        bool hasLen = false;
        bool notNull = false;
        bool primaryKey = false; // 列级主键（隐含 NOT NULL，由语义阶段补齐）
        // ── 约束族（收尾补齐）──────────────────────────────────
        bool hasDefault = false;               // 形如 DEFAULT <literal>
        std::unique_ptr<CELLA_Expr> defaultExpr;// 默认值表达式（解析期保证是字面量）
        bool unique = false;                   // 列级 UNIQUE
        bool hasCheck = false;                 // 列级 CHECK
        std::unique_ptr<CELLA_Expr> checkExpr; // CHECK 的谓词表达式
        bool hasReferences = false;            // 列级外键：REFERENCES t ( c )
        std::string refTable;                  // 被引用表
        std::vector<std::string> refColumns;   // 被引用列（省略时默认引用主键）
        std::string onDelete;                  // 级联动作（空 = RESTRICT）
        int line = 0, col = 0;
    };

    // 表级外键：FOREIGN KEY ( a [, b] ) REFERENCES t ( c [, d] ) [ON DELETE ...]
    struct CELLA_ForeignKey
    {
        std::vector<std::string> columns;    // 子表列（声明序）
        std::string refTable;                // 父表
        std::vector<std::string> refColumns; // 父表列（声明序；省略时默认父表主键）
        std::string onDelete;                // 空 = RESTRICT
        int line = 0, col = 0;
    };

    // 表级 CHECK：( expr )
    struct CELLA_TableCheck
    {
        std::unique_ptr<CELLA_Expr> expr;
        int line = 0, col = 0;
    };

    // 表级 UNIQUE：( a [, b] )
    struct CELLA_TableUnique
    {
        std::vector<std::string> columns;
        int line = 0, col = 0;
    };

    struct CELLA_TableRef
    {
        std::string name;
        std::string alias; // 可空
        int line = 0, col = 0;
    };

    enum class CELLA_JoinKind
    {
        MIDDLE,
        LEFT,
        RIGHT
    };

    struct CELLA_JoinClause
    {
        CELLA_JoinKind kind = CELLA_JoinKind::MIDDLE;
        CELLA_TableRef ref;
        std::unique_ptr<CELLA_Expr> on;
    };

    struct CELLA_SelectItem
    {
        std::unique_ptr<CELLA_Expr> expr;
        std::string alias; // 可空
    };

    struct CELLA_OrderItem
    {
        CELLA_ColName col;
        bool asc = true;
    };

    // ---------------- 语句 ----------------

    struct CELLA_Stmt
    {
        enum class Kind
        {
            CREATE_TABLE,
            INSERT,
            GET,
            DELETE,
            UPDATE,
            DROP_TABLE,
            CREATE_INDEX,
            DROP_INDEX,
            ALTER_TABLE,    // ALTER TABLE（P5：加列/删列/改名/主键）
            TRUNCATE_TABLE, // TRUNCATE TABLE（P5：快速清空）
            CREATE_VIEW,    // CREATE VIEW name AS <get>
            DROP_VIEW,      // DROP VIEW name
            WITH            // WITH n AS ( get ... ) <主语句>
        };

        // ALTER TABLE 的动作子类（P5）。一个语句只带一个动作 —— 与标准 SQL 一致，
        // 也让解析器不必为「多个逗号分隔动作」设计回滚语义。
        enum class AlterAction
        {
            ADD_COLUMN,       // ADD [COLUMN] <col> <type> [(len)] [NOT NULL]
            DROP_COLUMN,      // DROP [COLUMN] <col>
            RENAME_TABLE,     // RENAME TO <new_table>
            RENAME_COLUMN,    // RENAME COLUMN <old> TO <new>
            ADD_PRIMARY_KEY,  // ADD PRIMARY KEY ( <col> [, <col>]* )
            DROP_PRIMARY_KEY  // DROP PRIMARY KEY
        };

        Kind kind = Kind::CREATE_TABLE;
        int line = 0, col = 0;

        // CREATE TABLE / DELETE / UPDATE / DROP TABLE / ALTER TABLE / TRUNCATE TABLE 共用
        std::string tableName;

        // CREATE TABLE
        std::vector<CELLA_ColumnDef> columns;
        // 表级主键：PRIMARY KEY (a, b [, ...])。与列级主键互斥（SEM-313）；
        // 语义阶段把命中的列标记 primaryKey，后续流程（目录/执行/打印）复用列级机制。
        std::vector<std::string> tablePrimaryKey;
        int tablePkLine = 0, tablePkCol = 0;

        // CREATE TABLE：约束族（收尾补齐）
        std::vector<CELLA_TableCheck> tableChecks;      // 表级 CHECK
        std::vector<CELLA_TableUnique> tableUniques;    // 表级 UNIQUE
        std::vector<CELLA_ForeignKey> foreignKeys;      // 表级 FOREIGN KEY

        // CREATE VIEW / DROP VIEW
        std::string viewName;
        std::unique_ptr<CELLA_Stmt> viewQuery; // CREATE VIEW 的查询体（GET）

        // WITH（CTE）：WITH n1 AS ( get ... ) [, n2 AS ( ... )] <主语句>
        std::vector<std::string> cteNames;
        std::vector<std::unique_ptr<CELLA_Stmt>> cteQueries; // 与 cteNames 等长
        std::unique_ptr<CELLA_Stmt> cteMain;                 // WITH 之后的主语句

        // INSERT
        std::vector<std::string> insertColumns; // 空 = 省略列清单
        std::vector<std::vector<std::unique_ptr<CELLA_Expr>>> rows;

        // GET
        bool distinct = false;
        bool star = false;
        std::vector<CELLA_SelectItem> selectItems;
        CELLA_TableRef from;
        // FROM 子查询：FROM ( get ... ) [AS] alias [ ( c1, c2 ) ]
        std::unique_ptr<CELLA_Stmt> fromSubquery;
        std::string fromSubqueryAlias;
        std::vector<std::string> fromSubqueryColumns;
        std::vector<CELLA_JoinClause> joins;
        std::unique_ptr<CELLA_Expr> limit;      // = WHERE
        std::vector<CELLA_ColName> grouped;     // = GROUP BY
        std::unique_ptr<CELLA_Expr> having;     // = HAVING
        std::vector<CELLA_OrderItem> ordered;   // = ORDER BY
        long long among = -1;                   // = LIMIT 行数，-1 表示未指定
        bool hasPage = false;                   // = 分页（页码 1 起，每页行数默认 10）
        long long pageNo = 0;                   // 页码
        long long pageSize = 10;                // 每页行数
        std::unique_ptr<CELLA_Stmt> unionQuery; // UNION 右臂

        // UPDATE
        std::vector<std::pair<std::string, std::unique_ptr<CELLA_Expr>>> sets;

        // DELETE / UPDATE 共用
        std::unique_ptr<CELLA_Expr> where;

        // CREATE INDEX / DROP INDEX
        std::string indexName;   // 索引名（原始拼写）
        bool unique = false;     // CREATE UNIQUE INDEX 标志
        std::string indexColumn; // 被索引的列（单列 = 列名；复合 = "a,b" 逗号拼接，
                                 // 兼容既有打印/golden 输出；新代码请用 indexColumns）
        std::vector<std::string> indexColumns; // 复合索引的列清单（按声明序，≥1 项）

        // ── ALTER TABLE（P5）────────────────────────────────────
        AlterAction alterAction = AlterAction::ADD_COLUMN;
        CELLA_ColumnDef newColumn;            // ADD COLUMN：新列定义（复用列定义解析）
        std::string alterColumnName;          // DROP COLUMN / RENAME COLUMN 的源列名
        std::string newName;                  // RENAME TO 的新表名 / RENAME COLUMN 的新列名
        std::vector<std::string> pkColumns;   // ADD PRIMARY KEY ( ... ) 的主键列清单
    };

    struct CELLA_Program
    {
        std::vector<std::unique_ptr<CELLA_Stmt>> statements;
    };

    // ---------------- 深拷贝 ----------------
    // 计划/优化阶段需要持有表达式与语句副本。定义必须位于 CELLA_Stmt 之后 ——
    // 子查询字段（unique_ptr<CELLA_Stmt>）需要完整类型才能实例化析构。
    inline std::unique_ptr<CELLA_Stmt> cella_cloneStmt(const CELLA_Stmt &s);
    inline std::unique_ptr<CELLA_Expr> cella_cloneExpr(const CELLA_Expr &e);

    // 列定义深拷贝（CELLA_ColumnDef 因持有 defaultExpr/checkExpr 而不可复制）
    inline CELLA_ColumnDef cella_cloneColumnDef(const CELLA_ColumnDef &cd)
    {
        CELLA_ColumnDef c;
        c.name = cd.name;
        c.type = cd.type;
        c.len = cd.len;
        c.hasLen = cd.hasLen;
        c.notNull = cd.notNull;
        c.primaryKey = cd.primaryKey;
        c.hasDefault = cd.hasDefault;
        c.unique = cd.unique;
        c.hasCheck = cd.hasCheck;
        c.hasReferences = cd.hasReferences;
        c.refTable = cd.refTable;
        c.refColumns = cd.refColumns;
        c.onDelete = cd.onDelete;
        c.line = cd.line;
        c.col = cd.col;
        if (cd.defaultExpr)
            c.defaultExpr = cella_cloneExpr(*cd.defaultExpr);
        if (cd.checkExpr)
            c.checkExpr = cella_cloneExpr(*cd.checkExpr);
        return c;
    }

    inline std::unique_ptr<CELLA_Expr> cella_cloneExpr(const CELLA_Expr &e)
    {
        auto n = std::make_unique<CELLA_Expr>();
        n->kind = e.kind;
        n->line = e.line;
        n->col = e.col;
        n->lit = e.lit;
        n->text = e.text;
        n->num = e.num;
        n->boolVal = e.boolVal;
        n->table = e.table;
        n->column = e.column;
        n->uop = e.uop;
        n->bop = e.bop;
        n->aggStar = e.aggStar;
        n->aggFunc = e.aggFunc;
        n->funcName = e.funcName;
        n->negated = e.negated;
        n->winPartition = e.winPartition;
        n->winOrder = e.winOrder;
        n->winOrderAsc = e.winOrderAsc;
        if (e.left)
            n->left = cella_cloneExpr(*e.left);
        if (e.right)
            n->right = cella_cloneExpr(*e.right);
        if (e.child)
            n->child = cella_cloneExpr(*e.child);
        for (const auto &a : e.args)
            n->args.push_back(cella_cloneExpr(*a));
        for (const auto &a : e.inList)
            n->inList.push_back(cella_cloneExpr(*a));
        if (e.subquery)
            n->subquery = cella_cloneStmt(*e.subquery);
        return n;
    }

    inline std::unique_ptr<CELLA_Stmt> cella_cloneStmt(const CELLA_Stmt &s)
    {
        auto n = std::make_unique<CELLA_Stmt>();
        n->kind = s.kind;
        n->line = s.line;
        n->col = s.col;
        n->tableName = s.tableName;
        for (const auto &cd : s.columns)
            n->columns.push_back(cella_cloneColumnDef(cd));
        n->tablePrimaryKey = s.tablePrimaryKey;
        n->tablePkLine = s.tablePkLine;
        n->tablePkCol = s.tablePkCol;
        for (const auto &tc : s.tableChecks)
        {
            CELLA_TableCheck t;
            t.line = tc.line;
            t.col = tc.col;
            if (tc.expr)
                t.expr = cella_cloneExpr(*tc.expr);
            n->tableChecks.push_back(std::move(t));
        }
        n->tableUniques = s.tableUniques;
        n->foreignKeys = s.foreignKeys;
        n->viewName = s.viewName;
        if (s.viewQuery)
            n->viewQuery = cella_cloneStmt(*s.viewQuery);
        n->cteNames = s.cteNames;
        for (const auto &q : s.cteQueries)
            n->cteQueries.push_back(cella_cloneStmt(*q));
        if (s.cteMain)
            n->cteMain = cella_cloneStmt(*s.cteMain);
        n->insertColumns = s.insertColumns;
        for (const auto &row : s.rows)
        {
            std::vector<std::unique_ptr<CELLA_Expr>> r;
            for (const auto &e : row)
                r.push_back(cella_cloneExpr(*e));
            n->rows.push_back(std::move(r));
        }
        n->distinct = s.distinct;
        n->star = s.star;
        for (const auto &si : s.selectItems)
        {
            CELLA_SelectItem it;
            it.alias = si.alias;
            if (si.expr)
                it.expr = cella_cloneExpr(*si.expr);
            n->selectItems.push_back(std::move(it));
        }
        n->from = s.from;
        if (s.fromSubquery)
            n->fromSubquery = cella_cloneStmt(*s.fromSubquery);
        n->fromSubqueryAlias = s.fromSubqueryAlias;
        n->fromSubqueryColumns = s.fromSubqueryColumns;
        for (const auto &jc : s.joins)
        {
            CELLA_JoinClause j;
            j.kind = jc.kind;
            j.ref = jc.ref;
            if (jc.on)
                j.on = cella_cloneExpr(*jc.on);
            n->joins.push_back(std::move(j));
        }
        if (s.limit)
            n->limit = cella_cloneExpr(*s.limit);
        n->grouped = s.grouped;
        if (s.having)
            n->having = cella_cloneExpr(*s.having);
        n->ordered = s.ordered;
        n->among = s.among;
        n->hasPage = s.hasPage;
        n->pageNo = s.pageNo;
        n->pageSize = s.pageSize;
        if (s.unionQuery)
            n->unionQuery = cella_cloneStmt(*s.unionQuery);
        for (const auto &p : s.sets)
        {
            n->sets.emplace_back(p.first, p.second ? cella_cloneExpr(*p.second) : nullptr);
        }
        if (s.where)
            n->where = cella_cloneExpr(*s.where);
        n->indexName = s.indexName;
        n->unique = s.unique;
        n->indexColumn = s.indexColumn;
        n->indexColumns = s.indexColumns;
        n->alterAction = s.alterAction;
        n->newColumn = cella_cloneColumnDef(s.newColumn);
        n->alterColumnName = s.alterColumnName;
        n->newName = s.newName;
        n->pkColumns = s.pkColumns;
        return n;
    }

} // namespace cella
