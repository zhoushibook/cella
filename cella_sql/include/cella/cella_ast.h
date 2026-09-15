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

    // ---------------- 表达式 ----------------

    enum class CELLA_LiteralKind
    {
        NUMBER,
        STRING,
        DATE,
        NULL_LIT,
        BOOL_LIT
    };

    struct CELLA_Expr
    {
        enum class Kind
        {
            LITERAL,
            COLUMN_REF,
            UNARY,
            BINARY,
            AGGREGATE // COUNT(*) / COUNT(col) / SUM / AVG / MIN / MAX（P4 + 收尾补齐）
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

    struct CELLA_ColName
    {
        std::string table; // 可空
        std::string column;
        int line = 0, col = 0;
    };

    struct CELLA_OrderItem
    {
        CELLA_ColName col;
        bool asc = true;
    };

    // 表达式深拷贝（计划/优化阶段需要持有表达式副本）
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
        if (e.left)
            n->left = cella_cloneExpr(*e.left);
        if (e.right)
            n->right = cella_cloneExpr(*e.right);
        if (e.child)
            n->child = cella_cloneExpr(*e.child);
        return n;
    }

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
            TRUNCATE_TABLE  // TRUNCATE TABLE（P5：快速清空）
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

        // INSERT
        std::vector<std::string> insertColumns; // 空 = 省略列清单
        std::vector<std::vector<std::unique_ptr<CELLA_Expr>>> rows;

        // GET
        bool distinct = false;
        bool star = false;
        std::vector<CELLA_SelectItem> selectItems;
        CELLA_TableRef from;
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

} // namespace cella
