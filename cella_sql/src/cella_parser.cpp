// cella_parser.cpp —— 语法分析：递归下降 + 预测分析，实现任务书 2.4 节完整文法。
// 错误记为 SYN-201（含期望/实际）与 SYN-202（缺分号），并同步恢复到下一个 ';'，
// 保证多语句输入一次能报告多条语法错误。
#include "cella/cella_parser.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cella
{
    namespace
    {

        std::unique_ptr<CELLA_Expr> makeBinary(CELLA_Expr::BinOp op, const CELLA_Token &t,
                                               std::unique_ptr<CELLA_Expr> l,
                                               std::unique_ptr<CELLA_Expr> r)
        {
            auto e = std::make_unique<CELLA_Expr>();
            e->kind = CELLA_Expr::Kind::BINARY;
            e->line = t.line;
            e->col = t.col;
            e->bop = op;
            e->left = std::move(l);
            e->right = std::move(r);
            return e;
        }

        // 聚合函数关键字总表（收尾补齐：COUNT/SUM/AVG/MIN/MAX）。
        // 集中一处判定，避免 parsePrimary 与 parseAggregate 分头写死函数名。
        bool isAggregateKeyword(CELLA_Keyword kw)
        {
            return kw == CELLA_Keyword::COUNT || kw == CELLA_Keyword::SUM ||
                   kw == CELLA_Keyword::AVG || kw == CELLA_Keyword::MIN ||
                   kw == CELLA_Keyword::MAX;
        }

        std::unique_ptr<CELLA_Expr> makeUnary(CELLA_Expr::UnOp op, const CELLA_Token &t,
                                              std::unique_ptr<CELLA_Expr> child)
        {
            auto e = std::make_unique<CELLA_Expr>();
            e->kind = CELLA_Expr::Kind::UNARY;
            e->line = t.line;
            e->col = t.col;
            e->uop = op;
            e->child = std::move(child);
            return e;
        }

        // 标量函数调用节点（UPPER/LENGTH/SUBSTR/DATEDIFF...）与窗口函数共用
        std::unique_ptr<CELLA_Expr> makeFunction(const CELLA_Token &nameTok, std::string name,
                                                 std::vector<std::unique_ptr<CELLA_Expr>> args)
        {
            auto e = std::make_unique<CELLA_Expr>();
            e->kind = CELLA_Expr::Kind::FUNCTION;
            e->line = nameTok.line;
            e->col = nameTok.col;
            e->funcName = std::move(name);
            e->args = std::move(args);
            return e;
        }

        class CELLA_ParserImpl
        {
        public:
            CELLA_ParserImpl(const std::vector<CELLA_Token> &toks, std::vector<CELLA_Error> &errs)
                : toks(toks), errors(errs) {}

            std::unique_ptr<CELLA_Program> parseProgram()
            {
                auto program = std::make_unique<CELLA_Program>();
                while (!atEnd())
                {
                    auto stmt = parseStatement();
                    if (stmt)
                    {
                        program->statements.push_back(std::move(stmt));
                    }
                    else
                    {
                        syncToSemicolon();
                    }
                }
                return program;
            }

        private:
            const std::vector<CELLA_Token> &toks;
            std::vector<CELLA_Error> &errors;
            size_t pos = 0;

            // ---------------- Token 游标 ----------------

            const CELLA_Token &peek(size_t off = 0) const
            {
                size_t p = pos + off;
                return p < toks.size() ? toks[p] : toks.back();
            }
            const CELLA_Token &advance()
            {
                const CELLA_Token &t = toks[pos];
                if (pos + 1 < toks.size())
                    pos++;
                return t;
            }
            bool atEnd() const { return peek().type == CELLA_TokenType::EOF_T; }

            // 回溯支持：用于「'(' 后到底是子查询还是括起表达式」这类需要前瞻的分支。
            // 注意：reset 只回退游标，不回退 errors —— 试探路径不得产生诊断。
            size_t mark() const { return pos; }
            void reset(size_t m) { pos = m; }

            bool matchKw(CELLA_Keyword kw)
            {
                if (peek().type == CELLA_TokenType::KEYWORD && peek().keyword == kw)
                {
                    advance();
                    return true;
                }
                return false;
            }
            bool matchOp(const std::string &op)
            {
                if (peek().type == CELLA_TokenType::OPERATOR && peek().lexeme == op)
                {
                    advance();
                    return true;
                }
                return false;
            }
            bool matchDelim(const std::string &d)
            {
                if (peek().type == CELLA_TokenType::DELIMITER && peek().lexeme == d)
                {
                    advance();
                    return true;
                }
                return false;
            }

            // ---------------- 错误与恢复 ----------------

            static std::string tokenDesc(const CELLA_Token &t)
            {
                switch (t.type)
                {
                case CELLA_TokenType::EOF_T:
                    return "文件结束";
                case CELLA_TokenType::KEYWORD:
                    return "关键字 " + cella_keywordText(t.keyword);
                case CELLA_TokenType::IDENTIFIER:
                    return "标识符 " + t.lexeme;
                case CELLA_TokenType::CONST:
                    return "常量 " + t.lexeme;
                case CELLA_TokenType::OPERATOR:
                    return "操作符 " + t.lexeme;
                case CELLA_TokenType::DELIMITER:
                    return "分隔符 " + t.lexeme;
                default:
                    return "非法 Token \"" + t.lexeme + "\"";
                }
            }

            void synError(const CELLA_Token &t, const std::string &expected)
            {
                errors.push_back(cella_makeError(CELLA_Phase::SYN, "SYN-201", t.line, t.col,
                                                 "期望 " + expected + "，实际为 " + tokenDesc(t)));
            }

            void synMissingSemicolon(const CELLA_Token &t)
            {
                errors.push_back(cella_makeError(CELLA_Phase::SYN, "SYN-202", t.line, t.col,
                                                 "语句缺少分号 ';' 结尾"));
            }

            void syncToSemicolon()
            {
                while (!atEnd())
                {
                    const CELLA_Token &t = peek();
                    if (t.type == CELLA_TokenType::DELIMITER && t.lexeme == ";")
                    {
                        advance();
                        return;
                    }
                    advance();
                }
            }

            // ---------------- 基础 expect ----------------

            bool expectKw(CELLA_Keyword kw)
            {
                if (matchKw(kw))
                    return true;
                synError(peek(), "关键字 " + cella_keywordText(kw));
                return false;
            }
            bool expectOp(const std::string &op)
            {
                if (matchOp(op))
                    return true;
                synError(peek(), "操作符 '" + op + "'");
                return false;
            }
            bool expectDelim(const std::string &d)
            {
                if (matchDelim(d))
                    return true;
                synError(peek(), "分隔符 '" + d + "'");
                return false;
            }
            bool expectIdent(std::string &out)
            {
                const CELLA_Token &t = peek();
                if (t.type == CELLA_TokenType::IDENTIFIER)
                {
                    out = t.lexeme;
                    advance();
                    return true;
                }
                synError(t, "标识符");
                return false;
            }
            bool expectSemicolon()
            {
                const CELLA_Token &t = peek();
                if (t.type == CELLA_TokenType::DELIMITER && t.lexeme == ";")
                {
                    advance();
                    return true;
                }
                synMissingSemicolon(t);
                return false;
            }

            static std::unique_ptr<CELLA_Stmt> makeStmt(CELLA_Stmt::Kind k, const CELLA_Token &t)
            {
                auto s = std::make_unique<CELLA_Stmt>();
                s->kind = k;
                s->line = t.line;
                s->col = t.col;
                return s;
            }

            // ---------------- 语句 ----------------

            std::unique_ptr<CELLA_Stmt> parseStatement()
            {
                const CELLA_Token &t = peek();
                if (t.type == CELLA_TokenType::KEYWORD)
                {
                    switch (t.keyword)
                    {
                    case CELLA_Keyword::CREATE:
                        // CREATE INDEX / CREATE UNIQUE INDEX 优先判定
                        if (peek(1).type == CELLA_TokenType::KEYWORD &&
                            (peek(1).keyword == CELLA_Keyword::INDEX ||
                             peek(1).keyword == CELLA_Keyword::UNIQUE))
                            return parseCreateIndex();
                        if (peek(1).type == CELLA_TokenType::KEYWORD &&
                            peek(1).keyword == CELLA_Keyword::VIEW)
                            return parseCreateView();
                        return parseCreateTable();
                    case CELLA_Keyword::INSERT:
                        return parseInsert();
                    case CELLA_Keyword::GET:
                        return parseGet();
                    case CELLA_Keyword::DELETE:
                        return parseDelete();
                    case CELLA_Keyword::UPDATE:
                        return parseUpdate();
                    case CELLA_Keyword::DROP:
                        if (peek(1).type == CELLA_TokenType::KEYWORD &&
                            peek(1).keyword == CELLA_Keyword::INDEX)
                            return parseDropIndex();
                        if (peek(1).type == CELLA_TokenType::KEYWORD &&
                            peek(1).keyword == CELLA_Keyword::VIEW)
                            return parseDropView();
                        return parseDropTable();
                    case CELLA_Keyword::ALTER:
                        return parseAlterTable();
                    case CELLA_Keyword::TRUNCATE:
                        return parseTruncateTable();
                    case CELLA_Keyword::WITH:
                        return parseWith();
                    default:
                        break;
                    }
                }
                synError(t, "CREATE/INSERT/GET/DELETE/UPDATE/DROP/ALTER/TRUNCATE/WITH");
                return nullptr;
            }

            // CREATE TABLE name '(' column_def { ',' column_def } ')' ';'
            std::unique_ptr<CELLA_Stmt> parseCreateTable()
            {
                const CELLA_Token &t = advance(); // CREATE
                auto st = makeStmt(CELLA_Stmt::Kind::CREATE_TABLE, t);
                if (!expectKw(CELLA_Keyword::TABLE))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (!expectDelim("("))
                    return nullptr;
                for (;;)
                {
                    // 表级主键：PRIMARY KEY ( col { ',' col } )。只能出现在某个列定义之后
                    //（首位出现会因零列在语义阶段报「主键列不存在」），且之后只允许收尾。
                    if (peek().keyword == CELLA_Keyword::PRIMARY)
                    {
                        const CELLA_Token &pkTok = peek();
                        if (!st->tablePrimaryKey.empty())
                        {
                            synError(pkTok, "表级主键重复定义");
                            return nullptr;
                        }
                        advance(); // PRIMARY
                        if (!expectKw(CELLA_Keyword::KEY))
                            return nullptr;
                        if (!expectDelim("("))
                            return nullptr;
                        st->tablePkLine = pkTok.line;
                        st->tablePkCol = pkTok.col;
                        do
                        {
                            std::string col;
                            if (!expectIdent(col))
                                return nullptr;
                            st->tablePrimaryKey.push_back(std::move(col));
                        } while (matchDelim(","));
                        if (!expectDelim(")"))
                            return nullptr;
                        if (matchDelim(","))
                        {
                            synError(peek(), "表级主键之后不应再有列定义或约束");
                            return nullptr;
                        }
                        if (!expectDelim(")"))
                            return nullptr;
                        if (!expectSemicolon())
                            return nullptr;
                        return st;
                    }

                    // 表级 CHECK：( expr )
                    if (peek().keyword == CELLA_Keyword::CHECK)
                    {
                        const CELLA_Token &ck = peek();
                        advance();
                        CELLA_TableCheck tc;
                        tc.line = ck.line;
                        tc.col = ck.col;
                        if (!expectDelim("("))
                            return nullptr;
                        tc.expr = parseExpr();
                        if (!tc.expr)
                            return nullptr;
                        if (!expectDelim(")"))
                            return nullptr;
                        st->tableChecks.push_back(std::move(tc));
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                    // 表级 UNIQUE：( a [, b]* )
                    if (peek().keyword == CELLA_Keyword::UNIQUE)
                    {
                        const CELLA_Token &uk = peek();
                        advance();
                        CELLA_TableUnique tu;
                        tu.line = uk.line;
                        tu.col = uk.col;
                        if (!expectDelim("("))
                            return nullptr;
                        do
                        {
                            std::string c;
                            if (!expectIdent(c))
                                return nullptr;
                            tu.columns.push_back(std::move(c));
                        } while (matchDelim(","));
                        if (!expectDelim(")"))
                            return nullptr;
                        st->tableUniques.push_back(std::move(tu));
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                    // 表级外键：FOREIGN KEY ( a [, b]* ) REFERENCES t [ ( c [, d]* ) ] [ON DELETE ...]
                    if (peek().keyword == CELLA_Keyword::FOREIGN)
                    {
                        const CELLA_Token &fk = peek();
                        advance();
                        if (!expectKw(CELLA_Keyword::KEY))
                            return nullptr;
                        CELLA_ForeignKey f;
                        f.line = fk.line;
                        f.col = fk.col;
                        if (!expectDelim("("))
                            return nullptr;
                        do
                        {
                            std::string c;
                            if (!expectIdent(c))
                                return nullptr;
                            f.columns.push_back(std::move(c));
                        } while (matchDelim(","));
                        if (!expectDelim(")"))
                            return nullptr;
                        if (!expectKw(CELLA_Keyword::REFERENCES))
                            return nullptr;
                        if (!expectIdent(f.refTable))
                            return nullptr;
                        if (matchDelim("("))
                        {
                            do
                            {
                                std::string c;
                                if (!expectIdent(c))
                                    return nullptr;
                                f.refColumns.push_back(std::move(c));
                            } while (matchDelim(","));
                            if (!expectDelim(")"))
                                return nullptr;
                        }
                        parseOnDelete(f.onDelete);
                        st->foreignKeys.push_back(std::move(f));
                        if (matchDelim(","))
                            continue;
                        break;
                    }

                    CELLA_ColumnDef cd;
                    if (!parseColumnDef(cd))
                        return nullptr;
                    st->columns.push_back(std::move(cd));
                    if (matchDelim(","))
                        continue;
                    break;
                }
                if (!expectDelim(")"))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            bool parseDataType(CELLA_DataType &out)
            {
                const CELLA_Token &t = peek();
                if (t.type != CELLA_TokenType::KEYWORD)
                {
                    synError(t, "数据类型 INT/FLOAT/DOUBLE/CHAR/VARCHAR/TEXT/DATE/TIME/DATETIME/BOOL");
                    return false;
                }
                switch (t.keyword)
                {
                case CELLA_Keyword::INT:
                case CELLA_Keyword::INTEGER:
                    out = CELLA_DataType::INT;
                    break;
                case CELLA_Keyword::FLOAT:
                    out = CELLA_DataType::FLOAT;
                    break;
                case CELLA_Keyword::DOUBLE:
                    out = CELLA_DataType::DOUBLE;
                    break;
                case CELLA_Keyword::CHAR:
                    out = CELLA_DataType::CHAR;
                    break;
                case CELLA_Keyword::VARCHAR:
                    out = CELLA_DataType::VARCHAR;
                    break;
                case CELLA_Keyword::TEXT:
                    out = CELLA_DataType::TEXT;
                    break;
                case CELLA_Keyword::DATE:
                    out = CELLA_DataType::DATE;
                    break;
                case CELLA_Keyword::TIME:
                    out = CELLA_DataType::TIME;
                    break;
                case CELLA_Keyword::DATETIME:
                    out = CELLA_DataType::DATETIME;
                    break;
                case CELLA_Keyword::BOOL:
                case CELLA_Keyword::BOOLEAN:
                    out = CELLA_DataType::BOOL;
                    break;
                default:
                    synError(t, "数据类型 INT/FLOAT/DOUBLE/CHAR/VARCHAR/TEXT/DATE/TIME/DATETIME/BOOL");
                    return false;
                }
                advance();
                return true;
            }

            // 列定义：<col> <type> [ '(' len ')' ] { NOT NULL | PRIMARY KEY }
            // CREATE TABLE 与 ALTER TABLE ADD COLUMN 共用同一段（保证两处的列语义永不漂移）。
            bool parseColumnDef(CELLA_ColumnDef &cd)
            {
                const CELLA_Token &idTok = peek();
                if (!expectIdent(cd.name))
                    return false;
                cd.line = idTok.line;
                cd.col = idTok.col;
                if (!parseDataType(cd.type))
                    return false;
                // 仅 CHAR/VARCHAR 允许 '(n)'
                if ((cd.type == CELLA_DataType::CHAR || cd.type == CELLA_DataType::VARCHAR) &&
                    matchDelim("("))
                {
                    const CELLA_Token &lenTok = peek();
                    if (lenTok.type == CELLA_TokenType::CONST &&
                        lenTok.valueType == CELLA_TokenValueType::NUMBER &&
                        lenTok.lexeme.find('.') == std::string::npos)
                    {
                        advance();
                        cd.hasLen = true;
                        cd.len = static_cast<int>(lenTok.numValue);
                    }
                    else
                    {
                        synError(lenTok, "非负整数");
                        return false;
                    }
                    if (!expectDelim(")"))
                        return false;
                }
                // 列约束：NOT NULL / PRIMARY KEY / UNIQUE / DEFAULT <lit> / CHECK ( expr ) /
                //           REFERENCES t [ ( col ) ] [ ON DELETE CASCADE|RESTRICT ]
                // 可任意顺序、可重复出现（重复由语义阶段判定）。
                bool more_constraints = true;
                while (more_constraints)
                {
                    if (matchKw(CELLA_Keyword::NOT))
                    {
                        if (!expectKw(CELLA_KW_NULL))
                            return false;
                        cd.notNull = true;
                    }
                    else if (matchKw(CELLA_Keyword::PRIMARY))
                    {
                        if (!expectKw(CELLA_Keyword::KEY))
                            return false;
                        cd.primaryKey = true;
                    }
                    else if (matchKw(CELLA_Keyword::UNIQUE))
                    {
                        cd.unique = true;
                    }
                    else if (matchKw(CELLA_Keyword::DEFAULT))
                    {
                        cd.hasDefault = true;
                        cd.defaultExpr = parseLiteral();
                        if (!cd.defaultExpr)
                            return false;
                    }
                    else if (matchKw(CELLA_Keyword::CHECK))
                    {
                        cd.hasCheck = true;
                        if (!expectDelim("("))
                            return false;
                        cd.checkExpr = parseExpr();
                        if (!cd.checkExpr)
                            return false;
                        if (!expectDelim(")"))
                            return false;
                    }
                    else if (matchKw(CELLA_Keyword::REFERENCES))
                    {
                        cd.hasReferences = true;
                        if (!expectIdent(cd.refTable))
                            return false;
                        if (matchDelim("("))
                        {
                            for (;;)
                            {
                                std::string rc;
                                if (!expectIdent(rc))
                                    return false;
                                cd.refColumns.push_back(std::move(rc));
                                if (matchDelim(","))
                                    continue;
                                break;
                            }
                            if (!expectDelim(")"))
                                return false;
                        }
                        parseOnDelete(cd.onDelete);
                    }
                    else
                    {
                        more_constraints = false;
                    }
                }
                return true;
            }

            // [ ON DELETE CASCADE | RESTRICT ]（省略 = RESTRICT）
            void parseOnDelete(std::string &out)
            {
                const size_t save = mark();
                if (!matchKw(CELLA_Keyword::ON))
                {
                    reset(save);
                    return;
                }
                if (!matchKw(CELLA_Keyword::DELETE))
                {
                    reset(save);
                    return;
                }
                const CELLA_Token &t = peek();
                if (t.type == CELLA_TokenType::IDENTIFIER && cella_toUpper(t.lexeme) == "CASCADE")
                {
                    out = "CASCADE";
                    advance();
                }
                else if (t.type == CELLA_TokenType::IDENTIFIER && cella_toUpper(t.lexeme) == "RESTRICT")
                {
                    out = "RESTRICT";
                    advance();
                }
                else
                {
                    // 只写了 ON DELETE 却没给动作：保留为 RESTRICT 并放行（宽容处理）
                    out = "RESTRICT";
                }
            }

            // INSERT INTO name [ '(' cols ')' ] VALUES row { ',' row } ';'
            std::unique_ptr<CELLA_Stmt> parseInsert()
            {
                const CELLA_Token &t = advance(); // INSERT
                auto st = makeStmt(CELLA_Stmt::Kind::INSERT, t);
                if (!expectKw(CELLA_Keyword::INTO))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (matchDelim("("))
                {
                    for (;;)
                    {
                        std::string c;
                        if (!expectIdent(c))
                            return nullptr;
                        st->insertColumns.push_back(c);
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                    if (!expectDelim(")"))
                        return nullptr;
                }
                if (!expectKw(CELLA_Keyword::VALUES))
                    return nullptr;
                for (;;)
                {
                    if (!expectDelim("("))
                        return nullptr;
                    std::vector<std::unique_ptr<CELLA_Expr>> row;
                    for (;;)
                    {
                        // VALUES ( DEFAULT )：显式要求取该列默认值（无默认值时语义阶段报错）
                        if (peek().type == CELLA_TokenType::KEYWORD &&
                            peek().keyword == CELLA_Keyword::DEFAULT)
                        {
                            const CELLA_Token mk = peek();
                            advance();
                            auto d = std::make_unique<CELLA_Expr>();
                            d->kind = CELLA_Expr::Kind::LITERAL;
                            d->lit = CELLA_LiteralKind::DEFAULT_LIT;
                            d->line = mk.line;
                            d->col = mk.col;
                            row.push_back(std::move(d));
                        }
                        else
                        {
                            auto e = parseLiteral();
                            if (!e)
                                return nullptr;
                            row.push_back(std::move(e));
                        }
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                    if (!expectDelim(")"))
                        return nullptr;
                    st->rows.push_back(std::move(row));
                    if (matchDelim(","))
                        continue;
                    break;
                }
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // get [ distinct ] select_list in table_ref { join_clause }
            //     [ limit expr ] [ grouped cols ] [ having expr ]
            //     [ ordered items ] [ among uint ] [ union get_stmt ] ';'
            std::unique_ptr<CELLA_Stmt> parseGet()
            {
                const CELLA_Token &t = advance(); // GET
                auto st = makeStmt(CELLA_Stmt::Kind::GET, t);
                if (!parseGetBody(*st))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            bool parseGetBody(CELLA_Stmt &st)
            {
                if (matchKw(CELLA_Keyword::DISTINCT))
                    st.distinct = true;

                // select_list
                if (matchOp("*"))
                {
                    st.star = true;
                }
                else
                {
                    for (;;)
                    {
                        CELLA_SelectItem item;
                        item.expr = parseExpr();
                        if (!item.expr)
                            return false;
                        if (matchKw(CELLA_Keyword::AS))
                        {
                            if (!expectIdent(item.alias))
                                return false;
                        }
                        else if (peek().type == CELLA_TokenType::IDENTIFIER)
                        {
                            item.alias = peek().lexeme;
                            advance();
                        }
                        st.selectItems.push_back(std::move(item));
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                }

                if (!expectKw(CELLA_Keyword::IN))
                    return false;
                if (!parseTableRef(st.from))
                    return false;

                // join_clause { join_clause }
                for (;;)
                {
                    CELLA_JoinKind kind = CELLA_JoinKind::MIDDLE;
                    if (matchKw(CELLA_Keyword::LEFT))
                        kind = CELLA_JoinKind::LEFT;
                    else if (matchKw(CELLA_Keyword::RIGHT))
                        kind = CELLA_JoinKind::RIGHT;
                    else if (matchKw(CELLA_Keyword::MIDDLE))
                        kind = CELLA_JoinKind::MIDDLE;
                    else if (!(peek().type == CELLA_TokenType::KEYWORD &&
                               peek().keyword == CELLA_Keyword::JOIN))
                        break;
                    if (!expectKw(CELLA_Keyword::JOIN))
                        return false;
                    CELLA_JoinClause jc;
                    jc.kind = kind;
                    if (!parseTableRef(jc.ref))
                        return false;
                    if (!expectKw(CELLA_Keyword::ON))
                        return false;
                    jc.on = parseExpr();
                    if (!jc.on)
                        return false;
                    st.joins.push_back(std::move(jc));
                }

                if (matchKw(CELLA_Keyword::LIMIT))
                {
                    st.limit = parseExpr();
                    if (!st.limit)
                        return false;
                }
                if (matchKw(CELLA_Keyword::GROUPED))
                {
                    for (;;)
                    {
                        CELLA_ColName cn;
                        if (!parseColName(cn))
                            return false;
                        st.grouped.push_back(cn);
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                }
                if (matchKw(CELLA_Keyword::HAVING))
                {
                    st.having = parseExpr();
                    if (!st.having)
                        return false;
                }
                if (matchKw(CELLA_Keyword::ORDERED))
                {
                    for (;;)
                    {
                        CELLA_OrderItem oi;
                        if (!parseColName(oi.col))
                            return false;
                        if (matchKw(CELLA_Keyword::ASC))
                            oi.asc = true;
                        else if (matchKw(CELLA_Keyword::DESC))
                            oi.asc = false;
                        st.ordered.push_back(oi);
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                }
                if (matchKw(CELLA_Keyword::AMONG))
                {
                    const CELLA_Token &t = peek();
                    if (t.type == CELLA_TokenType::CONST && t.valueType == CELLA_TokenValueType::NUMBER &&
                        t.lexeme.find('.') == std::string::npos)
                    {
                        st.among = static_cast<long long>(t.numValue);
                        advance();
                    }
                    else
                    {
                        synError(t, "非负整数");
                        return false;
                    }
                }
                // 分页：page 页码 [ [ ',' ] 每页行数 ]（页码 1 起，每页行数默认 10）
                if (matchKw(CELLA_Keyword::PAGE))
                {
                    const CELLA_Token &t1 = peek();
                    if (t1.type == CELLA_TokenType::CONST && t1.valueType == CELLA_TokenValueType::NUMBER &&
                        t1.lexeme.find('.') == std::string::npos)
                    {
                        st.hasPage = true;
                        st.pageNo = static_cast<long long>(t1.numValue);
                        advance();
                    }
                    else
                    {
                        synError(t1, "正整数（页码）");
                        return false;
                    }
                    const CELLA_Token &next = peek();
                    const bool hasComma = matchDelim(",");
                    const bool hasSpaceSeparatedSize =
                        next.type == CELLA_TokenType::CONST && next.valueType == CELLA_TokenValueType::NUMBER &&
                        next.lexeme.find('.') == std::string::npos;
                    if (hasComma || hasSpaceSeparatedSize)
                    {
                        const CELLA_Token &t2 = peek();
                        if (t2.type == CELLA_TokenType::CONST && t2.valueType == CELLA_TokenValueType::NUMBER &&
                            t2.lexeme.find('.') == std::string::npos)
                        {
                            st.pageSize = static_cast<long long>(t2.numValue);
                            advance();
                        }
                        else
                        {
                            synError(t2, "正整数（每页行数）");
                            return false;
                        }
                    }
                }
                if (matchKw(CELLA_Keyword::UNION))
                {
                    const CELLA_Token &t = toks[pos - 1];
                    auto arm = std::make_unique<CELLA_Stmt>();
                    arm->kind = CELLA_Stmt::Kind::GET;
                    arm->line = t.line;
                    arm->col = t.col;
                    // 文法: union get_stmt —— 右臂本身是完整 get 语句，须以 get 开头
                    if (!expectKw(CELLA_Keyword::GET))
                        return false;
                    if (!parseGetBody(*arm))
                        return false;
                    st.unionQuery = std::move(arm);
                }
                return true;
            }

            bool parseTableRef(CELLA_TableRef &ref)
            {
                const CELLA_Token &t = peek();
                if (t.type != CELLA_TokenType::IDENTIFIER)
                {
                    synError(t, "表名");
                    return false;
                }
                advance();
                ref.name = t.lexeme;
                ref.line = t.line;
                ref.col = t.col;
                if (matchKw(CELLA_Keyword::AS))
                {
                    if (!expectIdent(ref.alias))
                        return false;
                }
                else if (peek().type == CELLA_TokenType::IDENTIFIER)
                {
                    ref.alias = peek().lexeme;
                    advance();
                }
                return true;
            }

            bool parseColName(CELLA_ColName &cn)
            {
                const CELLA_Token &t = peek();
                if (t.type != CELLA_TokenType::IDENTIFIER)
                {
                    synError(t, "列名");
                    return false;
                }
                advance();
                cn.column = t.lexeme;
                cn.line = t.line;
                cn.col = t.col;
                if (matchOp("."))
                {
                    cn.table = cn.column;
                    if (!expectIdent(cn.column))
                        return false;
                }
                return true;
            }

            // DELETE in table_name [ LIMIT expr ] ';'（FROM → in，WHERE → limit）
            std::unique_ptr<CELLA_Stmt> parseDelete()
            {
                const CELLA_Token &t = advance(); // DELETE
                auto st = makeStmt(CELLA_Stmt::Kind::DELETE, t);
                if (!expectKw(CELLA_Keyword::IN))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (matchKw(CELLA_Keyword::LIMIT))
                {
                    st->where = parseExpr();
                    if (!st->where)
                        return nullptr;
                }
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // UPDATE name SET col '=' expr { ',' col '=' expr } [ LIMIT expr ] ';'
            // 已彻底移除 WHERE，条件用 limit
            std::unique_ptr<CELLA_Stmt> parseUpdate()
            {
                const CELLA_Token &t = advance(); // UPDATE
                auto st = makeStmt(CELLA_Stmt::Kind::UPDATE, t);
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (!expectKw(CELLA_Keyword::SET))
                    return nullptr;
                for (;;)
                {
                    CELLA_ColName cn;
                    if (!parseColName(cn))
                        return nullptr;
                    if (!expectOp("="))
                        return nullptr;
                    auto e = parseExpr();
                    if (!e)
                        return nullptr;
                    st->sets.push_back({cn.column, std::move(e)});
                    if (matchDelim(","))
                        continue;
                    break;
                }
                if (matchKw(CELLA_Keyword::LIMIT))
                {
                    st->where = parseExpr();
                    if (!st->where)
                        return nullptr;
                }
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // DROP TABLE name ';'
            std::unique_ptr<CELLA_Stmt> parseDropTable()
            {
                const CELLA_Token &t = advance(); // DROP
                auto st = makeStmt(CELLA_Stmt::Kind::DROP_TABLE, t);
                if (!expectKw(CELLA_Keyword::TABLE))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // CREATE [UNIQUE] INDEX idx ON table '(' column [',' column]* ')' ';'
            // 复合索引 = 括号内逗号分隔的列清单；indexColumns 按声明序保存，
            // indexColumn 同步为逗号拼接（兼容既有打印与 golden 输出）。
            std::unique_ptr<CELLA_Stmt> parseCreateIndex()
            {
                const CELLA_Token &t = advance(); // CREATE
                auto st = makeStmt(CELLA_Stmt::Kind::CREATE_INDEX, t);
                if (matchKw(CELLA_Keyword::UNIQUE))
                    st->unique = true;
                if (!expectKw(CELLA_Keyword::INDEX))
                    return nullptr;
                if (!expectIdent(st->indexName))
                    return nullptr;
                if (!expectKw(CELLA_Keyword::ON))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (!expectDelim("("))
                    return nullptr;
                std::string first;
                if (!expectIdent(first))
                    return nullptr;
                st->indexColumns.push_back(first);
                while (matchDelim(","))
                {
                    std::string more;
                    if (!expectIdent(more))
                        return nullptr;
                    st->indexColumns.push_back(more);
                }
                if (!expectDelim(")"))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                // 拼接回 indexColumn（单列 = 原名，逐字节不变）
                for (size_t i = 0; i < st->indexColumns.size(); ++i)
                {
                    if (i > 0)
                        st->indexColumn += ",";
                    st->indexColumn += st->indexColumns[i];
                }
                return st;
            }

            // DROP INDEX idx ';'
            std::unique_ptr<CELLA_Stmt> parseDropIndex()
            {
                const CELLA_Token &t = advance(); // DROP
                auto st = makeStmt(CELLA_Stmt::Kind::DROP_INDEX, t);
                if (!expectKw(CELLA_Keyword::INDEX))
                    return nullptr;
                if (!expectIdent(st->indexName))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // ALTER TABLE name <action> ';'
            //   ADD [COLUMN] col type [ '(' len ')' ] [ NOT NULL ]
            //   DROP [COLUMN] col
            //   RENAME TO new_name
            //   RENAME COLUMN old TO new
            //   ADD PRIMARY KEY ( col { ',' col } )
            //   DROP PRIMARY KEY
            //
            // 一次只允许一个动作：标准 SQL 也允许多动作，但那会让「部分成功」的语义
            // 变得难以界定（本项目的 DDL 不做事务回滚），故按最小集合实现。
            // COLUMN 关键字可有可无（与 MySQL 兼容）；RENAME 后面必须跟 TO 或 COLUMN，
            // 便于把「表改名」与「列改名」区分开。
            std::unique_ptr<CELLA_Stmt> parseAlterTable()
            {
                const CELLA_Token &t = advance(); // ALTER
                auto st = makeStmt(CELLA_Stmt::Kind::ALTER_TABLE, t);
                if (!expectKw(CELLA_Keyword::TABLE))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;

                if (matchKw(CELLA_Keyword::ADD))
                {
                    if (matchKw(CELLA_Keyword::PRIMARY))
                    {
                        if (!expectKw(CELLA_Keyword::KEY))
                            return nullptr;
                        st->alterAction = CELLA_Stmt::AlterAction::ADD_PRIMARY_KEY;
                        if (!parsePkColumnList(st->pkColumns))
                            return nullptr;
                    }
                    else
                    {
                        (void)matchKw(CELLA_Keyword::COLUMN); // COLUMN 可省略
                        st->alterAction = CELLA_Stmt::AlterAction::ADD_COLUMN;
                        if (!parseColumnDef(st->newColumn))
                            return nullptr;
                    }
                }
                else if (matchKw(CELLA_Keyword::DROP))
                {
                    if (matchKw(CELLA_Keyword::PRIMARY))
                    {
                        if (!expectKw(CELLA_Keyword::KEY))
                            return nullptr;
                        st->alterAction = CELLA_Stmt::AlterAction::DROP_PRIMARY_KEY;
                    }
                    else
                    {
                        (void)matchKw(CELLA_Keyword::COLUMN); // COLUMN 可省略
                        st->alterAction = CELLA_Stmt::AlterAction::DROP_COLUMN;
                        if (!expectIdent(st->alterColumnName))
                            return nullptr;
                    }
                }
                else if (matchKw(CELLA_Keyword::RENAME))
                {
                    if (matchKw(CELLA_Keyword::TO))
                    {
                        st->alterAction = CELLA_Stmt::AlterAction::RENAME_TABLE;
                        if (!expectIdent(st->newName))
                            return nullptr;
                    }
                    else if (matchKw(CELLA_Keyword::COLUMN))
                    {
                        st->alterAction = CELLA_Stmt::AlterAction::RENAME_COLUMN;
                        if (!expectIdent(st->alterColumnName))
                            return nullptr;
                        if (!expectKw(CELLA_Keyword::TO))
                            return nullptr;
                        if (!expectIdent(st->newName))
                            return nullptr;
                    }
                    else
                    {
                        synError(peek(), "关键字 TO（表改名）或 COLUMN（列改名）");
                        return nullptr;
                    }
                }
                else
                {
                    synError(peek(), "ALTER TABLE 的动作 ADD / DROP / RENAME");
                    return nullptr;
                }
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // '(' col { ',' col } ')' —— 表级主键（CREATE TABLE）与 ADD PRIMARY KEY 共用
            bool parsePkColumnList(std::vector<std::string> &out)
            {
                if (!expectDelim("("))
                    return false;
                do
                {
                    std::string col;
                    if (!expectIdent(col))
                        return false;
                    out.push_back(std::move(col));
                } while (matchDelim(","));
                return expectDelim(")");
            }

            // TRUNCATE TABLE name ';'
            std::unique_ptr<CELLA_Stmt> parseTruncateTable()
            {
                const CELLA_Token &t = advance(); // TRUNCATE
                auto st = makeStmt(CELLA_Stmt::Kind::TRUNCATE_TABLE, t);
                if (!expectKw(CELLA_Keyword::TABLE))
                    return nullptr;
                if (!expectIdent(st->tableName))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // ---------------- 视图与 CTE ----------------

            // 子查询：以 get 开头、不带结尾分号的查询体
            bool parseSubquery(std::unique_ptr<CELLA_Stmt> *out)
            {
                const CELLA_Token &g = peek();
                if (!(g.type == CELLA_TokenType::KEYWORD && g.keyword == CELLA_Keyword::GET))
                {
                    synError(g, "关键字 GET（子查询）");
                    return false;
                }
                advance();
                auto st = makeStmt(CELLA_Stmt::Kind::GET, g);
                if (!parseGetBody(*st))
                    return false;
                *out = std::move(st);
                return true;
            }

            // CREATE VIEW name AS <get>
            std::unique_ptr<CELLA_Stmt> parseCreateView()
            {
                const CELLA_Token &t = advance(); // CREATE
                auto st = makeStmt(CELLA_Stmt::Kind::CREATE_VIEW, t);
                if (!expectKw(CELLA_Keyword::VIEW))
                    return nullptr;
                if (!expectIdent(st->viewName))
                    return nullptr;
                if (!expectKw(CELLA_Keyword::AS))
                    return nullptr;
                if (!parseSubquery(&st->viewQuery))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // DROP VIEW name
            std::unique_ptr<CELLA_Stmt> parseDropView()
            {
                const CELLA_Token &t = advance(); // DROP
                auto st = makeStmt(CELLA_Stmt::Kind::DROP_VIEW, t);
                if (!expectKw(CELLA_Keyword::VIEW))
                    return nullptr;
                if (!expectIdent(st->viewName))
                    return nullptr;
                if (!expectSemicolon())
                    return nullptr;
                return st;
            }

            // WITH n1 AS ( get ... ) [, n2 AS ( get ... )] <主语句>
            std::unique_ptr<CELLA_Stmt> parseWith()
            {
                const CELLA_Token &t = advance(); // WITH
                auto st = makeStmt(CELLA_Stmt::Kind::WITH, t);
                for (;;)
                {
                    std::string name;
                    if (!expectIdent(name))
                        return nullptr;
                    if (!expectKw(CELLA_Keyword::AS))
                        return nullptr;
                    if (!expectDelim("("))
                        return nullptr;
                    std::unique_ptr<CELLA_Stmt> q;
                    if (!parseSubquery(&q))
                        return nullptr;
                    if (!expectDelim(")"))
                        return nullptr;
                    st->cteNames.push_back(std::move(name));
                    st->cteQueries.push_back(std::move(q));
                    if (matchDelim(","))
                        continue;
                    break;
                }
                st->cteMain = parseStatement();
                if (!st->cteMain)
                    return nullptr;
                return st;
            }

            // ---------------- 表达式（任务书 2.4 节） ----------------

            std::unique_ptr<CELLA_Expr> parseExpr() { return parseOr(); }

            std::unique_ptr<CELLA_Expr> parseOr()
            {
                auto l = parseAnd();
                if (!l)
                    return nullptr;
                while (matchKw(CELLA_Keyword::OR))
                {
                    const CELLA_Token &op = toks[pos - 1];
                    auto r = parseAnd();
                    if (!r)
                        return nullptr;
                    l = makeBinary(CELLA_Expr::BinOp::OR, op, std::move(l), std::move(r));
                }
                return l;
            }

            std::unique_ptr<CELLA_Expr> parseAnd()
            {
                auto l = parseNot();
                if (!l)
                    return nullptr;
                while (matchKw(CELLA_Keyword::AND))
                {
                    const CELLA_Token &op = toks[pos - 1];
                    auto r = parseNot();
                    if (!r)
                        return nullptr;
                    l = makeBinary(CELLA_Expr::BinOp::AND, op, std::move(l), std::move(r));
                }
                return l;
            }

            std::unique_ptr<CELLA_Expr> parseNot()
            {
                if (matchKw(CELLA_Keyword::NOT))
                {
                    const CELLA_Token &op = toks[pos - 1];
                    // NOT EXISTS (...) 合成单个 negated 节点。若留给通用前缀 NOT，
                    // 会变成 NOT(EXISTS(...)) 双层，打印与求值都要额外处理。
                    if (peek().keyword == CELLA_Keyword::EXISTS)
                    {
                        auto c = parsePrimary();
                        if (!c)
                            return nullptr;
                        if (c->kind == CELLA_Expr::Kind::EXISTS_Q)
                        {
                            c->negated = !c->negated;
                            c->line = op.line;
                            c->col = op.col;
                            return c;
                        }
                    }
                    auto c = parseNot();
                    if (!c)
                        return nullptr;
                    return makeUnary(CELLA_Expr::UnOp::NOT, op, std::move(c));
                }
                return parseComparison();
            }

            std::unique_ptr<CELLA_Expr> parseComparison()
            {
                auto l = parseAdd();
                if (!l)
                    return nullptr;
                // 后缀判空：x IS [NOT] NULL。绑定到左侧（加法级）操作数上，
                // 结果恒为 TRUE/FALSE，不走三值比较 —— 这是判 NULL 的唯一合法写法
                // （x = NULL 恒为 UNKNOWN，查不出任何行）。
                if (peek().keyword == CELLA_Keyword::IS)
                {
                    const CELLA_Token isTok = peek();
                    advance();
                    bool negated = false;
                    if (peek().keyword == CELLA_Keyword::NOT)
                    {
                        negated = true;
                        advance();
                    }
                    if (peek().keyword != CELLA_Keyword::NULL)
                    {
                        errors.push_back(cella_makeError(
                            CELLA_Phase::SYN, "SYN-201", isTok.line, isTok.col,
                            negated ? "期望 关键字 NULL（判空写作 IS NOT NULL）"
                                    : "期望 关键字 NULL（判空写作 IS NULL）"));
                        return nullptr;
                    }
                    advance();
                    return makeUnary(negated ? CELLA_Expr::UnOp::IS_NOT_NULL
                                             : CELLA_Expr::UnOp::IS_NULL,
                                     isTok, std::move(l));
                }
                // 通配比较：x LIKE 'p' / x NOT LIKE 'p'。与 = / < 同级的二元比较符，
                // 所以 NOT LIKE 在解析期就合成单个 NOT_LIKE 节点 —— 若留给前缀 NOT 处理，
                // `x NOT LIKE p` 会因为 NOT 出现在中缀位置而解析失败。
                {
                    const CELLA_Token &cur = peek();
                    bool negated = false;
                    bool matched = false;
                    if (cur.keyword == CELLA_Keyword::LIKE)
                    {
                        matched = true;
                    }
                    else if (cur.keyword == CELLA_Keyword::NOT &&
                             peek(1).keyword == CELLA_Keyword::LIKE)
                    {
                        matched = true;
                        negated = true;
                        advance(); // 吃掉 NOT
                    }
                    if (matched)
                    {
                        const CELLA_Token likeTok = peek();
                        advance(); // 吃掉 LIKE
                        auto r = parseAdd();
                        if (!r)
                            return nullptr;
                        return makeBinary(negated ? CELLA_Expr::BinOp::NOT_LIKE
                                                  : CELLA_Expr::BinOp::LIKE,
                                          likeTok, std::move(l), std::move(r));
                    }
                }
                // 集合判定：x [NOT] IN ( v1, v2, ... ) / x [NOT] IN ( get ... )
                // 消歧关键：本方言用 "GET ... IN <表>" 表示 FROM，所以只有 IN 后面
                // 紧跟 '(' 时才当成集合判定，否则把 IN 留给上层子句处理。
                {
                    bool negated = false;
                    bool matched = false;
                    const CELLA_Token &cur = peek();
                    if (cur.keyword == CELLA_Keyword::IN && peek(1).type == CELLA_TokenType::DELIMITER &&
                        peek(1).lexeme == "(")
                    {
                        matched = true;
                    }
                    else if (cur.keyword == CELLA_Keyword::NOT &&
                             peek(1).keyword == CELLA_Keyword::IN &&
                             peek(2).type == CELLA_TokenType::DELIMITER && peek(2).lexeme == "(")
                    {
                        matched = true;
                        negated = true;
                        advance(); // 吃掉 NOT
                    }
                    if (matched)
                    {
                        const CELLA_Token inTok = peek();
                        advance(); // 吃掉 IN
                        auto e = std::make_unique<CELLA_Expr>();
                        e->kind = CELLA_Expr::Kind::IN_LIST;
                        e->line = inTok.line;
                        e->col = inTok.col;
                        e->negated = negated;
                        e->left = std::move(l);
                        if (!expectDelim("("))
                            return nullptr;
                        if (peek().type == CELLA_TokenType::KEYWORD && peek().keyword == CELLA_Keyword::GET)
                        {
                            e->kind = CELLA_Expr::Kind::IN_QUERY;
                            if (!parseSubquery(&e->subquery))
                                return nullptr;
                        }
                        else
                        {
                            for (;;)
                            {
                                auto v = parseExpr();
                                if (!v)
                                    return nullptr;
                                e->inList.push_back(std::move(v));
                                if (matchDelim(","))
                                    continue;
                                break;
                            }
                        }
                        if (!expectDelim(")"))
                            return nullptr;
                        return e;
                    }
                }
                const CELLA_Token &t = peek();
                CELLA_Expr::BinOp op = CELLA_Expr::BinOp::EQ;
                bool hasOp = false;
                if (t.type == CELLA_TokenType::OPERATOR)
                {
                    if (t.lexeme == "=" || t.lexeme == "==")
                    {
                        op = CELLA_Expr::BinOp::EQ;
                        hasOp = true;
                    }
                    else if (t.lexeme == "!=" || t.lexeme == "<>")
                    {
                        op = CELLA_Expr::BinOp::NE;
                        hasOp = true;
                    }
                    else if (t.lexeme == "<")
                    {
                        op = CELLA_Expr::BinOp::LT;
                        hasOp = true;
                    }
                    else if (t.lexeme == "<=")
                    {
                        op = CELLA_Expr::BinOp::LE;
                        hasOp = true;
                    }
                    else if (t.lexeme == ">")
                    {
                        op = CELLA_Expr::BinOp::GT;
                        hasOp = true;
                    }
                    else if (t.lexeme == ">=")
                    {
                        op = CELLA_Expr::BinOp::GE;
                        hasOp = true;
                    }
                }
                if (!hasOp)
                    return l;
                advance();
                auto r = parseAdd();
                if (!r)
                    return nullptr;
                return makeBinary(op, t, std::move(l), std::move(r));
            }

            std::unique_ptr<CELLA_Expr> parseAdd()
            {
                auto l = parseMul();
                if (!l)
                    return nullptr;
                for (;;)
                {
                    const CELLA_Token &t = peek();
                    CELLA_Expr::BinOp op;
                    if (t.type == CELLA_TokenType::OPERATOR && t.lexeme == "+")
                        op = CELLA_Expr::BinOp::PLUS;
                    else if (t.type == CELLA_TokenType::OPERATOR && t.lexeme == "-")
                        op = CELLA_Expr::BinOp::MINUS;
                    else
                        return l;
                    advance();
                    auto r = parseMul();
                    if (!r)
                        return nullptr;
                    l = makeBinary(op, t, std::move(l), std::move(r));
                }
            }

            std::unique_ptr<CELLA_Expr> parseMul()
            {
                auto l = parseUnary();
                if (!l)
                    return nullptr;
                for (;;)
                {
                    const CELLA_Token &t = peek();
                    CELLA_Expr::BinOp op;
                    if (t.type == CELLA_TokenType::OPERATOR && t.lexeme == "*")
                        op = CELLA_Expr::BinOp::MUL;
                    else if (t.type == CELLA_TokenType::OPERATOR && t.lexeme == "/")
                        op = CELLA_Expr::BinOp::DIV;
                    else
                        return l;
                    advance();
                    auto r = parseUnary();
                    if (!r)
                        return nullptr;
                    l = makeBinary(op, t, std::move(l), std::move(r));
                }
            }

            std::unique_ptr<CELLA_Expr> parseUnary()
            {
                if (matchOp("-"))
                {
                    const CELLA_Token &op = toks[pos - 1];
                    auto c = parseUnary();
                    if (!c)
                        return nullptr;
                    return makeUnary(CELLA_Expr::UnOp::NEG, op, std::move(c));
                }
                return parsePrimary();
            }

            // 窗口子句：OVER ( [PARTITION [BY] c1, ...] [ORDERED [BY] c1 [ASC|DESC], ...] )
            // 方言里 BY 可省略（与 grouped / ordered 保持一致），故此处按可选词处理。
            bool parseWindowSpec(CELLA_Expr &e)
            {
                if (!expectDelim("("))
                    return false;
                if (peek().keyword == CELLA_Keyword::PARTITION)
                {
                    advance();
                    if (peek().keyword == CELLA_Keyword::BY)
                        advance();
                    for (;;)
                    {
                        CELLA_ColName cn;
                        const CELLA_Token &ct = peek();
                        cn.line = ct.line;
                        cn.col = ct.col;
                        if (!expectIdent(cn.column))
                            return false;
                        if (matchOp("."))
                        {
                            cn.table = cn.column;
                            if (!expectIdent(cn.column))
                                return false;
                        }
                        e.winPartition.push_back(cn);
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                }
                if (peek().keyword == CELLA_Keyword::ORDERED)
                {
                    advance();
                    if (peek().keyword == CELLA_Keyword::BY)
                        advance();
                    for (;;)
                    {
                        CELLA_ColName cn;
                        const CELLA_Token &ct = peek();
                        cn.line = ct.line;
                        cn.col = ct.col;
                        if (!expectIdent(cn.column))
                            return false;
                        if (matchOp("."))
                        {
                            cn.table = cn.column;
                            if (!expectIdent(cn.column))
                                return false;
                        }
                        e.winOrder.push_back(cn);
                        bool asc = true;
                        if (peek().keyword == CELLA_Keyword::DESC)
                        {
                            asc = false;
                            advance();
                        }
                        else if (peek().keyword == CELLA_Keyword::ASC)
                        {
                            advance();
                        }
                        e.winOrderAsc.push_back(asc);
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                }
                return expectDelim(")");
            }

            // 通用函数调用：name ( [arg[, arg]...] ) [ OVER ( ... ) ]
            // 命中函数表才当成函数调用，否则回退为列引用 —— 这样把函数名的判定
            // 收敛到 cella_common.h 的单一规格表，不会出现「解析器认、执行器不认」。
            std::unique_ptr<CELLA_Expr> parseFunctionCall()
            {
                const CELLA_Token fn = peek();
                auto e = std::make_unique<CELLA_Expr>();
                e->kind = CELLA_Expr::Kind::FUNCTION;
                e->line = fn.line;
                e->col = fn.col;
                e->funcName = cella_toUpper(fn.lexeme);
                advance(); // 函数名
                if (!expectDelim("("))
                    return nullptr;
                if (!matchDelim(")"))
                {
                    for (;;)
                    {
                        auto a = parseExpr();
                        if (!a)
                            return nullptr;
                        e->args.push_back(std::move(a));
                        if (matchDelim(","))
                            continue;
                        break;
                    }
                    if (!expectDelim(")"))
                        return nullptr;
                }
                if (peek().keyword == CELLA_Keyword::OVER)
                {
                    advance();
                    if (!parseWindowSpec(*e))
                        return nullptr;
                    e->kind = CELLA_Expr::Kind::WINDOW;
                }
                return e;
            }

            std::unique_ptr<CELLA_Expr> parsePrimary()
            {
                const CELLA_Token &t = peek();
                if (t.state != CELLA_TokenState::VALID)
                {
                    synError(t, "合法 Token（前一词法错误导致）");
                    return nullptr;
                }
                if (t.type == CELLA_TokenType::CONST ||
                    (t.type == CELLA_TokenType::KEYWORD &&
                     (t.keyword == CELLA_KW_NULL || t.keyword == CELLA_Keyword::TRUE ||
                      t.keyword == CELLA_Keyword::FALSE)))
                {
                    return parseLiteral();
                }
                if (t.type == CELLA_TokenType::KEYWORD && isAggregateKeyword(t.keyword))
                {
                    auto agg = parseAggregate();
                    if (!agg)
                        return nullptr;
                    // 聚合 + OVER → 窗口聚合：保留 aggFunc/column，只改 kind
                    if (peek().keyword == CELLA_Keyword::OVER)
                    {
                        advance();
                        if (!parseWindowSpec(*agg))
                            return nullptr;
                        agg->kind = CELLA_Expr::Kind::WINDOW;
                    }
                    return agg;
                }
                // EXISTS ( get ... )
                if (t.type == CELLA_TokenType::KEYWORD && t.keyword == CELLA_Keyword::EXISTS)
                {
                    advance();
                    auto e = std::make_unique<CELLA_Expr>();
                    e->kind = CELLA_Expr::Kind::EXISTS_Q;
                    e->line = t.line;
                    e->col = t.col;
                    if (!expectDelim("("))
                        return nullptr;
                    if (!parseSubquery(&e->subquery))
                        return nullptr;
                    if (!expectDelim(")"))
                        return nullptr;
                    return e;
                }
                // 函数调用优先于列引用：name 后紧跟 '(' 且 name 在函数规格表里。
                // 未命中则按普通标识符走列引用分支，因此不会误伤同名列。
                if (t.type == CELLA_TokenType::IDENTIFIER && peek(1).type == CELLA_TokenType::DELIMITER &&
                    peek(1).lexeme == "(" && cella_findFunc(cella_toUpper(t.lexeme)) != nullptr)
                {
                    return parseFunctionCall();
                }
                if (t.type == CELLA_TokenType::IDENTIFIER)
                {
                    auto e = std::make_unique<CELLA_Expr>();
                    e->kind = CELLA_Expr::Kind::COLUMN_REF;
                    e->line = t.line;
                    e->col = t.col;
                    e->column = t.lexeme;
                    advance();
                    if (matchOp("."))
                    {
                        e->table = e->column;
                        if (!expectIdent(e->column))
                            return nullptr;
                    }
                    return e;
                }
                if (t.type == CELLA_TokenType::DELIMITER && t.lexeme == "(")
                {
                    advance();
                    // 标量子查询：( get ... ) —— 必须抢在普通括号表达式之前判定，
                    // 否则 GET 会被当成缺失操作数报 SYN-201。
                    if (peek().type == CELLA_TokenType::KEYWORD && peek().keyword == CELLA_Keyword::GET)
                    {
                        auto e = std::make_unique<CELLA_Expr>();
                        e->kind = CELLA_Expr::Kind::SCALAR_Q;
                        e->line = t.line;
                        e->col = t.col;
                        if (!parseSubquery(&e->subquery))
                            return nullptr;
                        if (!expectDelim(")"))
                            return nullptr;
                        return e;
                    }
                    auto e = parseExpr();
                    if (!e)
                        return nullptr;
                    if (!expectDelim(")"))
                        return nullptr;
                    return e;
                }
                synError(t, "表达式");
                return nullptr;
            }

            // 聚合函数调用：<fn> ( * ) | <fn> ( [表.]列 )
            // fn ∈ { COUNT, SUM, AVG, MIN, MAX }；函数名原文（大写）写入 aggFunc，
            // 后续语义/计划/打印/执行全部按该字符串参数化分派。
            // "*" 只在解析层放行，是否合法由语义阶段判定（只有 COUNT(*) 合法）。
            std::unique_ptr<CELLA_Expr> parseAggregate()
            {
                const CELLA_Token &fn = peek();
                auto e = std::make_unique<CELLA_Expr>();
                e->kind = CELLA_Expr::Kind::AGGREGATE;
                e->line = fn.line;
                e->col = fn.col;
                e->aggFunc = cella_keywordText(fn.keyword);
                advance(); // 吃掉函数名
                if (!expectDelim("("))
                    return nullptr;
                if (matchOp("*"))
                {
                    e->aggStar = true;
                    if (!expectDelim(")"))
                        return nullptr;
                    return e;
                }
                if (!expectIdent(e->column))
                    return nullptr;
                if (matchOp("."))
                {
                    e->table = e->column;
                    if (!expectIdent(e->column))
                        return nullptr;
                }
                if (!expectDelim(")"))
                    return nullptr;
                return e;
            }

            // 常量：NUMBER | STRING | DATE | NULL | TRUE | FALSE
            std::unique_ptr<CELLA_Expr> parseLiteral()
            {
                const CELLA_Token &t = peek();
                if (t.state != CELLA_TokenState::VALID)
                {
                    synError(t, "合法常量（前一词法错误导致）");
                    return nullptr;
                }
                auto e = std::make_unique<CELLA_Expr>();
                e->kind = CELLA_Expr::Kind::LITERAL;
                e->line = t.line;
                e->col = t.col;
                if (t.type == CELLA_TokenType::CONST)
                {
                    switch (t.valueType)
                    {
                    case CELLA_TokenValueType::NUMBER:
                        e->lit = CELLA_LiteralKind::NUMBER;
                        e->text = t.textValue;
                        e->num = t.numValue;
                        break;
                    case CELLA_TokenValueType::DATE:
                        e->lit = CELLA_LiteralKind::DATE;
                        e->text = t.textValue;
                        break;
                    case CELLA_TokenValueType::STRING:
                        e->lit = CELLA_LiteralKind::STRING;
                        e->text = t.textValue;
                        break;
                    default:
                        synError(t, "常量");
                        return nullptr;
                    }
                    advance();
                    return e;
                }
                if (t.type == CELLA_TokenType::KEYWORD)
                {
                    if (t.keyword == CELLA_KW_NULL)
                    {
                        e->lit = CELLA_LiteralKind::NULL_LIT;
                        advance();
                        return e;
                    }
                    if (t.keyword == CELLA_Keyword::TRUE)
                    {
                        e->lit = CELLA_LiteralKind::BOOL_LIT;
                        e->boolVal = true;
                        advance();
                        return e;
                    }
                    if (t.keyword == CELLA_Keyword::FALSE)
                    {
                        e->lit = CELLA_LiteralKind::BOOL_LIT;
                        e->boolVal = false;
                        advance();
                        return e;
                    }
                }
                synError(t, "常量（数字/字符串/日期/NULL/TRUE/FALSE）");
                return nullptr;
            }
        };

    } // namespace

    std::unique_ptr<CELLA_Program> cella_parse(const std::vector<CELLA_Token> &tokens,
                                               std::vector<CELLA_Error> &errors)
    {
        CELLA_ParserImpl parser(tokens, errors);
        return parser.parseProgram();
    }

} // namespace cella
