// cella_printer.cpp —— 文本输出：Token 流 / AST / 执行计划（任务书 5/7 节格式）。
// 同一输入输出稳定，供 golden 回归。
#include "cella/cella_printer.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace cella
{
    namespace
    {

        // ---------------- 表达式文本 ----------------

        // 语句打印（定义在后）；子查询内联展示复用同一实现，避免两套格式漂移
        void printStmt(const CELLA_Stmt &st, std::ostream &os, int level);
        // 限定名文本（定义在后；表达式打印需要）
        std::string colNameText(const CELLA_ColName &cn);

        // 把语句压成单行文本：走 printStmt 再把换行与缩进折叠为单个空格。
        // 这样内联子查询的文本与独立打印逐字一致，golden 不会出现双份格式。
        std::string inlineStmtText(const CELLA_Stmt &st)
        {
            std::ostringstream raw;
            printStmt(st, raw, 0);
            std::string s = raw.str();
            std::string out;
            bool pending_space = false;
            for (char c : s)
            {
                if (c == '\n')
                {
                    pending_space = true;
                    continue;
                }
                if (pending_space)
                {
                    if (c != ' ')
                        out += ' ';
                    pending_space = false;
                }
                out += c;
            }
            return out;
        }

        std::string opText(CELLA_Expr::BinOp op)
        {
            switch (op)
            {
            case CELLA_Expr::BinOp::EQ:
                return "=";
            case CELLA_Expr::BinOp::NE:
                return "!=";
            case CELLA_Expr::BinOp::LT:
                return "<";
            case CELLA_Expr::BinOp::LE:
                return "<=";
            case CELLA_Expr::BinOp::GT:
                return ">";
            case CELLA_Expr::BinOp::GE:
                return ">=";
            case CELLA_Expr::BinOp::PLUS:
                return "+";
            case CELLA_Expr::BinOp::MINUS:
                return "-";
            case CELLA_Expr::BinOp::MUL:
                return "*";
            case CELLA_Expr::BinOp::DIV:
                return "/";
            case CELLA_Expr::BinOp::AND:
                return "AND";
            case CELLA_Expr::BinOp::OR:
                return "OR";
            case CELLA_Expr::BinOp::LIKE:
                return "LIKE";
            case CELLA_Expr::BinOp::NOT_LIKE:
                return "NOT LIKE";
            }
            return "?";
        }

        int exprPrec(const CELLA_Expr &e)
        {
            switch (e.kind)
            {
            case CELLA_Expr::Kind::LITERAL:
            case CELLA_Expr::Kind::COLUMN_REF:
            case CELLA_Expr::Kind::AGGREGATE:
            case CELLA_Expr::Kind::FUNCTION:
            case CELLA_Expr::Kind::SCALAR_Q:
            case CELLA_Expr::Kind::WINDOW:
                return 7;
            case CELLA_Expr::Kind::UNARY:
                return 6;
            case CELLA_Expr::Kind::IN_LIST:
            case CELLA_Expr::Kind::IN_QUERY:
            case CELLA_Expr::Kind::EXISTS_Q:
                return 3;
            case CELLA_Expr::Kind::BINARY:
                switch (e.bop)
                {
                case CELLA_Expr::BinOp::OR:
                    return 1;
                case CELLA_Expr::BinOp::AND:
                    return 2;
                case CELLA_Expr::BinOp::EQ:
                case CELLA_Expr::BinOp::NE:
                case CELLA_Expr::BinOp::LT:
                case CELLA_Expr::BinOp::LE:
                case CELLA_Expr::BinOp::GT:
                case CELLA_Expr::BinOp::GE:
                case CELLA_Expr::BinOp::LIKE:
                case CELLA_Expr::BinOp::NOT_LIKE:
                    return 3;
                case CELLA_Expr::BinOp::PLUS:
                case CELLA_Expr::BinOp::MINUS:
                    return 4;
                case CELLA_Expr::BinOp::MUL:
                case CELLA_Expr::BinOp::DIV:
                    return 5;
                }
                break;
            }
            return 7;
        }

        std::string quoteEscape(const std::string &s)
        {
            std::string r;
            for (char c : s)
            {
                r += c;
                if (c == '\'')
                    r += '\''; // '' 转义还原
            }
            return r;
        }

        std::string exprToStringMin(const CELLA_Expr &e, int minPrec)
        {
            std::string s;
            switch (e.kind)
            {
            case CELLA_Expr::Kind::LITERAL:
                switch (e.lit)
                {
                case CELLA_LiteralKind::NUMBER:
                    s = e.text;
                    break;
                case CELLA_LiteralKind::STRING:
                    s = "'" + quoteEscape(e.text) + "'";
                    break;
                case CELLA_LiteralKind::DATE:
                    s = "'" + e.text + "'";
                    break;
                case CELLA_LiteralKind::NULL_LIT:
                    s = "NULL";
                    break;
                case CELLA_LiteralKind::BOOL_LIT:
                    s = e.boolVal ? "TRUE" : "FALSE";
                    break;
                case CELLA_LiteralKind::DEFAULT_LIT:
                    s = "DEFAULT";
                    break;
                }
                break;
            case CELLA_Expr::Kind::COLUMN_REF:
                s = e.table.empty() ? e.column : e.table + "." + e.column;
                break;
            case CELLA_Expr::Kind::UNARY:
            {
                std::string inner = exprToStringMin(*e.child, 6);
                switch (e.uop)
                {
                case CELLA_Expr::UnOp::NEG:
                    s = "-" + inner;
                    break;
                case CELLA_Expr::UnOp::NOT:
                    s = "NOT " + inner;
                    break;
                case CELLA_Expr::UnOp::IS_NULL:
                    s = inner + " IS NULL"; // 后缀判空：整体即谓词，优先级等同 UNARY
                    break;
                case CELLA_Expr::UnOp::IS_NOT_NULL:
                    s = inner + " IS NOT NULL";
                    break;
                }
                break;
            }
            case CELLA_Expr::Kind::AGGREGATE:
            {
                std::string fn = e.aggFunc.empty() ? "COUNT" : e.aggFunc;
                if (e.aggStar)
                    s = fn + "(*)";
                else
                    s = fn + "(" + (e.table.empty() ? e.column : e.table + "." + e.column) + ")";
                break;
            }
            case CELLA_Expr::Kind::BINARY:
            {
                int p = exprPrec(e);
                s = exprToStringMin(*e.left, p) + " " + opText(e.bop) + " " +
                    exprToStringMin(*e.right, p + 1);
                break;
            }
            case CELLA_Expr::Kind::FUNCTION:
            {
                s = e.funcName + "(";
                for (size_t i = 0; i < e.args.size(); i++)
                {
                    if (i != 0)
                        s += ", ";
                    s += exprToStringMin(*e.args[i], 0);
                }
                s += ")";
                break;
            }
            case CELLA_Expr::Kind::WINDOW:
            {
                // 两种形态共用同一节点：
                //   ① 排名函数 ROW_NUMBER() OVER (...)  → funcName 非空、args 空
                //   ② 窗口聚合 SUM(x) OVER (...)        → funcName 空、aggFunc/column 非空
                // 名字取二者之一，否则窗口聚合会打印成 "()"。
                const std::string wname = e.funcName.empty() ? e.aggFunc : e.funcName;
                s = wname + "(";
                if (!e.aggFunc.empty())
                {
                    s += e.aggStar ? "*" : (e.table.empty() ? e.column : e.table + "." + e.column);
                }
                else
                {
                    for (size_t i = 0; i < e.args.size(); i++)
                    {
                        if (i != 0)
                            s += ", ";
                        s += exprToStringMin(*e.args[i], 0);
                    }
                }
                s += ") OVER (";
                bool first = true;
                if (!e.winPartition.empty())
                {
                    s += "PARTITION BY ";
                    for (size_t i = 0; i < e.winPartition.size(); i++)
                    {
                        if (i != 0)
                            s += ", ";
                        s += colNameText(e.winPartition[i]);
                    }
                    first = false;
                }
                if (!e.winOrder.empty())
                {
                    if (!first)
                        s += " ";
                    s += "ORDERED BY ";
                    for (size_t i = 0; i < e.winOrder.size(); i++)
                    {
                        if (i != 0)
                            s += ", ";
                        s += colNameText(e.winOrder[i]);
                        const bool asc = (i < e.winOrderAsc.size()) ? e.winOrderAsc[i] : true;
                        s += asc ? " ASC" : " DESC";
                    }
                }
                s += ")";
                break;
            }
            case CELLA_Expr::Kind::IN_LIST:
            case CELLA_Expr::Kind::IN_QUERY:
            {
                s = exprToStringMin(*e.left, 4) + (e.negated ? " NOT IN (" : " IN (");
                if (e.kind == CELLA_Expr::Kind::IN_QUERY && e.subquery)
                {
                    s += inlineStmtText(*e.subquery);
                }
                else
                {
                    for (size_t i = 0; i < e.inList.size(); i++)
                    {
                        if (i != 0)
                            s += ", ";
                        s += exprToStringMin(*e.inList[i], 0);
                    }
                }
                s += ")";
                break;
            }
            case CELLA_Expr::Kind::EXISTS_Q:
            {
                s = std::string(e.negated ? "NOT EXISTS (" : "EXISTS (") +
                    (e.subquery ? inlineStmtText(*e.subquery) : std::string()) + ")";
                break;
            }
            case CELLA_Expr::Kind::SCALAR_Q:
            {
                s = "(" + (e.subquery ? inlineStmtText(*e.subquery) : std::string()) + ")";
                break;
            }
            }
            if (exprPrec(e) < minPrec)
                s = "(" + s + ")";
            return s;
        }

        // ---------------- 公共工具 ----------------

        std::string indent(int level) { return std::string(static_cast<size_t>(level) * 2, ' '); }

        std::string columnTypeText(const CELLA_ColumnDef &cd)
        {
            std::string s = cella_typeName(cd.type);
            if (cd.type == CELLA_DataType::CHAR || cd.type == CELLA_DataType::VARCHAR)
            {
                s += "(" + std::to_string(cd.hasLen ? cd.len : 255) + ")";
            }
            if (cd.notNull)
                s += " NOT NULL";
            if (cd.primaryKey)
                s += " PRIMARY KEY";
            if (cd.unique)
                s += " UNIQUE";
            if (cd.hasDefault && cd.defaultExpr)
                s += " DEFAULT " + exprToStringMin(*cd.defaultExpr, 0);
            if (cd.hasCheck && cd.checkExpr)
                s += " CHECK (" + exprToStringMin(*cd.checkExpr, 0) + ")";
            if (cd.hasReferences)
            {
                s += " REFERENCES " + cd.refTable;
                if (!cd.refColumns.empty())
                {
                    s += "(";
                    for (size_t i = 0; i < cd.refColumns.size(); i++)
                    {
                        if (i != 0)
                            s += ", ";
                        s += cd.refColumns[i];
                    }
                    s += ")";
                }
                if (!cd.onDelete.empty())
                    s += " ON DELETE " + cd.onDelete;
            }
            return s;
        }

        std::string colNameText(const CELLA_ColName &cn)
        {
            return cn.table.empty() ? cn.column : cn.table + "." + cn.column;
        }

        // ---------------- AST 打印 ----------------

        void printStmt(const CELLA_Stmt &st, std::ostream &os, int level);

        void printStmt(const CELLA_Stmt &st, std::ostream &os, int level)
        {
            switch (st.kind)
            {
            case CELLA_Stmt::Kind::CREATE_TABLE:
                os << indent(level) << "CreateTableStmt @" << st.line << ":" << st.col
                   << "  name=" << st.tableName << "\n";
                os << indent(level + 1) << "columns:\n";
                for (const auto &cd : st.columns)
                {
                    os << indent(level + 2) << cd.name << "  " << columnTypeText(cd) << "\n";
                }
                // 表级约束（收尾补齐）：仅在存在时打印，既有用例输出逐字节不变
                if (!st.tableUniques.empty() || !st.tableChecks.empty() || !st.foreignKeys.empty())
                {
                    os << indent(level + 1) << "constraints:\n";
                    for (const auto &tu : st.tableUniques)
                    {
                        os << indent(level + 2) << "UNIQUE (";
                        for (size_t i = 0; i < tu.columns.size(); i++)
                        {
                            if (i != 0)
                                os << ", ";
                            os << tu.columns[i];
                        }
                        os << ")\n";
                    }
                    for (const auto &tc : st.tableChecks)
                    {
                        os << indent(level + 2) << "CHECK ("
                           << (tc.expr ? exprToStringMin(*tc.expr, 0) : std::string()) << ")\n";
                    }
                    for (const auto &f : st.foreignKeys)
                    {
                        os << indent(level + 2) << "FOREIGN KEY (";
                        for (size_t i = 0; i < f.columns.size(); i++)
                        {
                            if (i != 0)
                                os << ", ";
                            os << f.columns[i];
                        }
                        os << ") REFERENCES " << f.refTable;
                        if (!f.refColumns.empty())
                        {
                            os << " (";
                            for (size_t i = 0; i < f.refColumns.size(); i++)
                            {
                                if (i != 0)
                                    os << ", ";
                                os << f.refColumns[i];
                            }
                            os << ")";
                        }
                        if (!f.onDelete.empty())
                            os << " ON DELETE " << f.onDelete;
                        os << "\n";
                    }
                }
                break;
            case CELLA_Stmt::Kind::INSERT:
                os << indent(level) << "InsertStmt @" << st.line << ":" << st.col
                   << "  table=" << st.tableName;
                if (!st.insertColumns.empty())
                {
                    os << "  columns=[";
                    for (size_t i = 0; i < st.insertColumns.size(); i++)
                    {
                        if (i)
                            os << ",";
                        os << st.insertColumns[i];
                    }
                    os << "]";
                }
                os << "\n";
                os << indent(level + 1) << "rows:\n";
                for (const auto &row : st.rows)
                {
                    os << indent(level + 2) << "(";
                    for (size_t i = 0; i < row.size(); i++)
                    {
                        if (i)
                            os << ",";
                        os << cella_exprToString(*row[i]);
                    }
                    os << ")\n";
                }
                break;
            case CELLA_Stmt::Kind::GET:
            {
                os << indent(level) << "GetStmt @" << st.line << ":" << st.col << "  select=[";
                if (st.star)
                {
                    if (st.distinct)
                        os << "DISTINCT ";
                    os << "*";
                }
                else
                {
                    if (st.distinct)
                        os << "DISTINCT ";
                    for (size_t i = 0; i < st.selectItems.size(); i++)
                    {
                        if (i)
                            os << ",";
                        os << cella_exprToString(*st.selectItems[i].expr);
                        if (!st.selectItems[i].alias.empty())
                        {
                            os << " as " << st.selectItems[i].alias;
                        }
                    }
                }
                os << "] from=[" << st.from.name;
                if (!st.from.alias.empty())
                    os << " " << st.from.alias;
                os << "]\n";
                for (const auto &jc : st.joins)
                {
                    std::string kindText = jc.kind == CELLA_JoinKind::MIDDLE
                                               ? "middle"
                                               : (jc.kind == CELLA_JoinKind::LEFT ? "left" : "right");
                    os << indent(level + 1) << "join: " << kindText << " " << jc.ref.name;
                    if (!jc.ref.alias.empty())
                        os << " " << jc.ref.alias;
                    os << " on " << cella_exprToString(*jc.on) << "\n";
                }
                if (st.limit)
                {
                    os << indent(level + 1) << "limit: " << cella_exprToString(*st.limit) << "\n";
                }
                if (!st.grouped.empty())
                {
                    os << indent(level + 1) << "grouped: ";
                    for (size_t i = 0; i < st.grouped.size(); i++)
                    {
                        if (i)
                            os << ", ";
                        os << colNameText(st.grouped[i]);
                    }
                    os << "\n";
                }
                if (st.having)
                {
                    os << indent(level + 1) << "having: " << cella_exprToString(*st.having) << "\n";
                }
                if (!st.ordered.empty())
                {
                    os << indent(level + 1) << "ordered: ";
                    for (size_t i = 0; i < st.ordered.size(); i++)
                    {
                        if (i)
                            os << ", ";
                        os << colNameText(st.ordered[i].col)
                           << (st.ordered[i].asc ? " ASC" : " DESC");
                    }
                    os << "\n";
                }
                if (st.among >= 0)
                {
                    os << indent(level + 1) << "among: " << st.among << "\n";
                }
                if (st.hasPage)
                {
                    os << indent(level + 1) << "page: " << st.pageNo << ", " << st.pageSize << "\n";
                }
                if (st.unionQuery)
                {
                    os << indent(level + 1) << "union:\n";
                    printStmt(*st.unionQuery, os, level + 2);
                }
                break;
            }
            case CELLA_Stmt::Kind::DELETE:
                os << indent(level) << "DeleteStmt @" << st.line << ":" << st.col
                   << "  table=" << st.tableName << "\n";
                if (st.where)
                {
                    os << indent(level + 1) << "limit: " << cella_exprToString(*st.where) << "\n";
                }
                break;
            case CELLA_Stmt::Kind::UPDATE:
                os << indent(level) << "UpdateStmt @" << st.line << ":" << st.col
                   << "  table=" << st.tableName << "\n";
                for (const auto &s : st.sets)
                {
                    os << indent(level + 1) << "set: " << s.first << " = "
                       << cella_exprToString(*s.second) << "\n";
                }
                if (st.where)
                {
                    os << indent(level + 1) << "limit: " << cella_exprToString(*st.where) << "\n";
                }
                break;
            case CELLA_Stmt::Kind::DROP_TABLE:
                os << indent(level) << "DropTableStmt @" << st.line << ":" << st.col
                   << "  table=" << st.tableName << "\n";
                break;
            case CELLA_Stmt::Kind::CREATE_INDEX:
                os << indent(level) << "CreateIndexStmt @" << st.line << ":" << st.col
                   << "  name=" << st.indexName << "  table=" << st.tableName
                   << "  column=" << st.indexColumn
                   << "  unique=" << (st.unique ? "true" : "false") << "\n";
                break;
            case CELLA_Stmt::Kind::DROP_INDEX:
                os << indent(level) << "DropIndexStmt @" << st.line << ":" << st.col
                   << "  name=" << st.indexName << "\n";
                break;
            case CELLA_Stmt::Kind::ALTER_TABLE:
                os << indent(level) << "AlterTableStmt @" << st.line << ":" << st.col
                   << "  table=" << st.tableName;
                switch (st.alterAction)
                {
                case CELLA_Stmt::AlterAction::ADD_COLUMN:
                    os << "  action=ADD COLUMN  column=" << st.newColumn.name << " "
                       << columnTypeText(st.newColumn) << "\n";
                    break;
                case CELLA_Stmt::AlterAction::DROP_COLUMN:
                    os << "  action=DROP COLUMN  column=" << st.alterColumnName << "\n";
                    break;
                case CELLA_Stmt::AlterAction::RENAME_TABLE:
                    os << "  action=RENAME TO  new_name=" << st.newName << "\n";
                    break;
                case CELLA_Stmt::AlterAction::RENAME_COLUMN:
                    os << "  action=RENAME COLUMN  column=" << st.alterColumnName
                       << "  new_name=" << st.newName << "\n";
                    break;
                case CELLA_Stmt::AlterAction::ADD_PRIMARY_KEY:
                    os << "  action=ADD PRIMARY KEY  columns=[";
                    for (size_t i = 0; i < st.pkColumns.size(); i++)
                    {
                        if (i)
                            os << ",";
                        os << st.pkColumns[i];
                    }
                    os << "]\n";
                    break;
                case CELLA_Stmt::AlterAction::DROP_PRIMARY_KEY:
                    os << "  action=DROP PRIMARY KEY\n";
                    break;
                }
                break;
            case CELLA_Stmt::Kind::TRUNCATE_TABLE:
                os << indent(level) << "TruncateTableStmt @" << st.line << ":" << st.col
                   << "  table=" << st.tableName << "\n";
                break;
            case CELLA_Stmt::Kind::CREATE_VIEW:
                os << indent(level) << "CreateViewStmt @" << st.line << ":" << st.col
                   << "  name=" << st.viewName << "\n";
                if (st.viewQuery)
                {
                    os << indent(level + 1) << "query:\n";
                    printStmt(*st.viewQuery, os, level + 2);
                }
                break;
            case CELLA_Stmt::Kind::DROP_VIEW:
                os << indent(level) << "DropViewStmt @" << st.line << ":" << st.col
                   << "  name=" << st.viewName << "\n";
                break;
            case CELLA_Stmt::Kind::WITH:
                os << indent(level) << "WithStmt @" << st.line << ":" << st.col << "\n";
                for (size_t i = 0; i < st.cteNames.size(); i++)
                {
                    os << indent(level + 1) << "cte " << st.cteNames[i] << ":\n";
                    if (i < st.cteQueries.size() && st.cteQueries[i])
                        printStmt(*st.cteQueries[i], os, level + 2);
                }
                if (st.cteMain)
                {
                    os << indent(level + 1) << "main:\n";
                    printStmt(*st.cteMain, os, level + 2);
                }
                break;
            }
        }

        void printPlanNode(const CELLA_PlanNode &n, std::ostream &os, int level)
        {
            os << indent(level) << n.op;
            if (n.op == "Filter" && n.pred)
                os << " (" << cella_exprToString(*n.pred) << ")";
            else if (n.op == "Join" && n.onExpr)
                os << " (" << n.joinKind << " on " << cella_exprToString(*n.onExpr) << ")";
            else if (!n.detail.empty())
                os << " " << n.detail;
            os << "\n";
            for (const auto &line : n.extra)
                os << indent(level + 1) << line << "\n";
            for (const auto &c : n.children)
                printPlanNode(*c, os, level + 1);
        }

    } // namespace

    std::string cella_exprToString(const CELLA_Expr &e) { return exprToStringMin(e, 0); }

    void cella_printTokens(const std::vector<CELLA_Token> &tokens, std::ostream &os)
    {
        for (const auto &t : tokens)
        {
            if (t.type == CELLA_TokenType::EOF_T)
            {
                os << "EOF\n";
                continue;
            }
            std::string typeText;
            switch (t.type)
            {
            case CELLA_TokenType::UNKNOWN:
                typeText = "UNKNOWN";
                break;
            case CELLA_TokenType::KEYWORD:
                typeText = "KEYWORD";
                break;
            case CELLA_TokenType::IDENTIFIER:
                typeText = "IDENTIFIER";
                break;
            case CELLA_TokenType::CONST:
                typeText = "CONST(" + cella_valueTypeText(t.valueType) + ")";
                break;
            case CELLA_TokenType::OPERATOR:
                typeText = "OPERATOR";
                break;
            case CELLA_TokenType::DELIMITER:
                typeText = "DELIMITER";
                break;
            default:
                typeText = "EOF";
                break;
            }
            std::string lexemeText =
                (t.type == CELLA_TokenType::KEYWORD) ? cella_keywordText(t.keyword) : t.lexeme;
            os << t.line << ":" << t.col << "  " << std::left << std::setw(14) << typeText << " "
               << lexemeText << "\n";
        }
    }

    void cella_printAst(const CELLA_Program &program, std::ostream &os)
    {
        os << "Program\n";
        for (const auto &st : program.statements)
            printStmt(*st, os, 1);
    }

    void cella_printPlan(const std::vector<std::unique_ptr<CELLA_PlanNode>> &plans, std::ostream &os)
    {
        bool first = true;
        for (const auto &p : plans)
        {
            if (!first)
                os << "\n";
            first = false;
            printPlanNode(*p, os, 0);
        }
    }

} // namespace cella
