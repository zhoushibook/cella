// cella_semantic.cpp —— 语义分析：基于内存 Catalog 检查表/列/类型一致性（任务书 3.3 节）。
// 每条语句独立检查：出错输出 SEM-3xx 诊断并标记该语句失败，继续处理后续语句；
// CREATE TABLE / DROP TABLE 成功时更新 Catalog。
#include "cella/cella_semantic.h"

#include <set>
#include <string>
#include <vector>

namespace cella
{
    namespace
    {

        // 查询作用域：from 表与各 join 表（支持别名）
        struct CELLA_ScopeEntry
        {
            std::string name;  // 表名（原始拼写）
            std::string alias; // 可空
        };

        const CELLA_Table *entryTable(const CELLA_Catalog &cat, const CELLA_ScopeEntry &e)
        {
            return cat.findTable(e.name);
        }

        // rowid：每张表都有的**只读伪列**（物理行标识）。可在投影 / 条件 / 排序里引用，
        // 但不能作为列名声明、不能出现在 INSERT 列清单或 UPDATE 的 SET 目标里
        //（后两者走 CELLA_Catalog::findColumn，天然找不到 rowid）。
        bool isRowidName(const std::string &name)
        {
            return cella_toUpper(name) == "ROWID";
        }

        bool tableHasColumn(const CELLA_Table &t, const std::string &name)
        {
            if (isRowidName(name))
                return true; // 伪列：每张表都有
            std::string key = cella_toUpper(name);
            for (const auto &c : t.columns)
            {
                if (cella_toUpper(c.name) == key)
                    return true;
            }
            return false;
        }

        // ---------------- 表达式类型系统（任务书 v2 14.1 / guide 5.3） ----------------

        // 表达式值类型分类
        enum class CELLA_ValueType
        {
            INT,
            FLOAT,
            DOUBLE,
            CHAR,
            VARCHAR,
            TEXT,
            DATE,
            TIME,
            DATETIME,
            BOOL,
            NULL_T,
            UNKNOWN
        };

        bool isNumericType(CELLA_ValueType t)
        {
            return t == CELLA_ValueType::INT || t == CELLA_ValueType::FLOAT || t == CELLA_ValueType::DOUBLE;
        }
        bool isStringType(CELLA_ValueType t)
        {
            return t == CELLA_ValueType::CHAR || t == CELLA_ValueType::VARCHAR || t == CELLA_ValueType::TEXT;
        }
        bool isDateTimeType(CELLA_ValueType t)
        {
            return t == CELLA_ValueType::DATE || t == CELLA_ValueType::TIME || t == CELLA_ValueType::DATETIME;
        }

        std::string valueTypeName(CELLA_ValueType t)
        {
            switch (t)
            {
            case CELLA_ValueType::INT:
                return "INT";
            case CELLA_ValueType::FLOAT:
                return "FLOAT";
            case CELLA_ValueType::DOUBLE:
                return "DOUBLE";
            case CELLA_ValueType::CHAR:
                return "CHAR";
            case CELLA_ValueType::VARCHAR:
                return "VARCHAR";
            case CELLA_ValueType::TEXT:
                return "TEXT";
            case CELLA_ValueType::DATE:
                return "DATE";
            case CELLA_ValueType::TIME:
                return "TIME";
            case CELLA_ValueType::DATETIME:
                return "DATETIME";
            case CELLA_ValueType::BOOL:
                return "BOOL";
            case CELLA_ValueType::NULL_T:
                return "NULL";
            case CELLA_ValueType::UNKNOWN:
                return "UNKNOWN";
            }
            return "?";
        }

        CELLA_ValueType dataTypeToValue(CELLA_DataType t)
        {
            switch (t)
            {
            case CELLA_DataType::INT:
                return CELLA_ValueType::INT;
            case CELLA_DataType::FLOAT:
                return CELLA_ValueType::FLOAT;
            case CELLA_DataType::DOUBLE:
                return CELLA_ValueType::DOUBLE;
            case CELLA_DataType::CHAR:
                return CELLA_ValueType::CHAR;
            case CELLA_DataType::VARCHAR:
                return CELLA_ValueType::VARCHAR;
            case CELLA_DataType::TEXT:
                return CELLA_ValueType::TEXT;
            case CELLA_DataType::DATE:
                return CELLA_ValueType::DATE;
            case CELLA_DataType::TIME:
                return CELLA_ValueType::TIME;
            case CELLA_DataType::DATETIME:
                return CELLA_ValueType::DATETIME;
            case CELLA_DataType::BOOL:
                return CELLA_ValueType::BOOL;
            }
            return CELLA_ValueType::UNKNOWN;
        }

        CELLA_ValueType promoteNumeric(CELLA_ValueType a, CELLA_ValueType b)
        {
            if (a == CELLA_ValueType::DOUBLE || b == CELLA_ValueType::DOUBLE)
                return CELLA_ValueType::DOUBLE;
            if (a == CELLA_ValueType::FLOAT || b == CELLA_ValueType::FLOAT)
                return CELLA_ValueType::FLOAT;
            return CELLA_ValueType::INT;
        }

        std::string binOpText(CELLA_Expr::BinOp op)
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

        // 在作用域内查找列定义（用于取列类型；歧义已在 resolveColumn 中报错）
        const CELLA_Column *scopeFindColumn(const std::vector<CELLA_ScopeEntry> &scope,
                                            const CELLA_Catalog &cat, const std::string &table,
                                            const std::string &column)
        {
            if (!table.empty())
            {
                for (const auto &se : scope)
                {
                    if (cella_toUpper(se.alias) == cella_toUpper(table) ||
                        cella_toUpper(se.name) == cella_toUpper(table))
                    {
                        const CELLA_Table *t = entryTable(cat, se);
                        return t ? CELLA_Catalog::findColumn(*t, column) : nullptr;
                    }
                }
                return nullptr;
            }
            for (const auto &se : scope)
            {
                const CELLA_Table *t = entryTable(cat, se);
                if (!t)
                    continue;
                const CELLA_Column *c = CELLA_Catalog::findColumn(*t, column);
                if (c)
                    return c;
            }
            return nullptr;
        }

        // 解析（可能带限定的）列引用；line/col 用于错误定位
        bool resolveColumn(const std::string &table, const std::string &column, int line, int col,
                           const std::vector<CELLA_ScopeEntry> &scope, const CELLA_Catalog &cat,
                           std::vector<CELLA_Error> &errors)
        {
            if (table.empty())
            {
                const CELLA_Table *found = nullptr;
                for (const auto &se : scope)
                {
                    const CELLA_Table *t = entryTable(cat, se);
                    if (t && tableHasColumn(*t, column))
                    {
                        if (found && found != t)
                        {
                            errors.push_back(cella_makeError(
                                CELLA_Phase::SEM, "SEM-308", line, col,
                                "列 \"" + column + "\" 不明确（表 \"" + found->name + "\" 与表 \"" +
                                    t->name + "\" 均含此列），请用 表名.列名 限定"));
                            return false;
                        }
                        found = t;
                    }
                }
                if (!found)
                {
                    std::string msg;
                    if (scope.size() == 1)
                    {
                        msg = "列 \"" + column + "\" 不存在于表 \"" + scope[0].name + "\"";
                    }
                    else
                    {
                        msg = "列 \"" + column + "\" 不存在于查询涉及的表";
                    }
                    errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", line, col, msg));
                    return false;
                }
                return true;
            }

            const CELLA_Table *target = nullptr;
            for (const auto &se : scope)
            {
                if (cella_toUpper(se.alias) == cella_toUpper(table) ||
                    cella_toUpper(se.name) == cella_toUpper(table))
                {
                    target = entryTable(cat, se);
                    break;
                }
            }
            if (!target)
            {
                errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", line, col,
                                                 "列 \"" + table + "." + column + "\" 引用的表 \"" + table +
                                                     "\" 不在查询范围"));
                return false;
            }
            if (!tableHasColumn(*target, column))
            {
                errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", line, col,
                                                 "列 \"" + column + "\" 不存在于表 \"" + target->name + "\""));
                return false;
            }
            return true;
        }

        // 语句级语义分析（定义在文件末尾，子查询需要递归调用它）
        bool analyzeStmt(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res);

        // 列引用校验（定义在下方，窗口子句需要提前用到）
        bool resolveColName(const CELLA_ColName &cn, const std::vector<CELLA_ScopeEntry> &scope,
                            const CELLA_Catalog &cat, std::vector<CELLA_Error> &errors);

        // 标量/窗口函数的结果类型。
        // ABS/ROUND 的结果依赖于实参类型，此处保守取 DOUBLE —— 结果类型只用于
        // 条件表达式的 BOOL 判定与列类型推导，不参与存储，保守取值不会造成错误放行。
        CELLA_ValueType scalarFuncResultType(const std::string &name)
        {
            if (name == "UPPER" || name == "LOWER" || name == "SUBSTR" || name == "SUBSTRING" ||
                name == "TRIM" || name == "LTRIM" || name == "RTRIM" || name == "REPLACE" ||
                name == "CONCAT" || name == "DATE_ADD" || name == "DATE_SUB")
                return CELLA_ValueType::TEXT;
            if (name == "ABS" || name == "ROUND")
                return CELLA_ValueType::DOUBLE;
            return CELLA_ValueType::INT; // LENGTH/CHAR_LENGTH/YEAR/MONTH/DAY/DATEDIFF/CEIL/FLOOR
        }

        // 子查询：在目录副本上跑完整的语句级分析。
        // 用副本是为了避免子查询里的 DDL 副作用污染外层目录；诊断并入外层 errors。
        // 已知边界：不做相关子查询（子查询内引用外层列会报 SEM-303），演示口径足够。
        bool resolveSubqueryExpr(const CELLA_Expr &e, const std::vector<CELLA_ScopeEntry> &scope,
                                 const CELLA_Catalog &cat, std::vector<CELLA_Error> &errors)
        {
            (void)scope;
            if (!e.subquery)
            {
                errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-340", e.line, e.col,
                                                 "子查询缺少语句体"));
                return false;
            }
            CELLA_Catalog sub = cat;
            CELLA_SemanticResult res;
            if (!analyzeStmt(*e.subquery, sub, 0, res))
            {
                for (const auto &err : res.errors)
                    errors.push_back(err);
                errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-341", e.line, e.col,
                                                 "子查询语义检查未通过"));
                return false;
            }
            return true;
        }

        // 标量子查询的结果类型：取首个选择项的类型。
        // 聚合项按 COUNT/SUM 的既有规则推导；其余返回 UNKNOWN（由外层比较决定相容性）。
        CELLA_ValueType subqueryScalarType(const CELLA_Expr &e)
        {
            if (!e.subquery || e.subquery->selectItems.empty())
                return CELLA_ValueType::UNKNOWN;
            const CELLA_SelectItem &it = e.subquery->selectItems[0];
            if (!it.expr)
                return CELLA_ValueType::UNKNOWN;
            if (it.expr->kind == CELLA_Expr::Kind::AGGREGATE)
            {
                return (it.expr->aggFunc == "AVG") ? CELLA_ValueType::DOUBLE
                                                   : CELLA_ValueType::INT;
            }
            if (it.expr->kind == CELLA_Expr::Kind::COLUMN_REF)
                return CELLA_ValueType::UNKNOWN; // 列类型需子查询作用域，交由执行期确定
            return scalarFuncResultType(it.expr->funcName);
        }

        // 存在性 + 类型检查；成功时通过 outType 输出表达式类型
        bool resolveExpr(const CELLA_Expr &e, const std::vector<CELLA_ScopeEntry> &scope,
                         const CELLA_Catalog &cat, std::vector<CELLA_Error> &errors,
                         CELLA_ValueType *outType = nullptr)
        {
            CELLA_ValueType t = CELLA_ValueType::UNKNOWN;
            switch (e.kind)
            {
            case CELLA_Expr::Kind::LITERAL:
                switch (e.lit)
                {
                case CELLA_LiteralKind::NUMBER:
                    t = e.text.find('.') != std::string::npos ? CELLA_ValueType::FLOAT
                                                              : CELLA_ValueType::INT;
                    break;
                case CELLA_LiteralKind::STRING:
                    t = CELLA_ValueType::VARCHAR;
                    break;
                case CELLA_LiteralKind::DATE:
                    t = CELLA_ValueType::DATE;
                    break;
                case CELLA_LiteralKind::NULL_LIT:
                    t = CELLA_ValueType::NULL_T;
                    break;
                case CELLA_LiteralKind::BOOL_LIT:
                    t = CELLA_ValueType::BOOL;
                    break;
                }
                break;
            case CELLA_Expr::Kind::COLUMN_REF:
            {
                if (!resolveColumn(e.table, e.column, e.line, e.col, scope, cat, errors))
                    return false;
                const CELLA_Column *c = scopeFindColumn(scope, cat, e.table, e.column);
                if (c == nullptr && isRowidName(e.column))
                    t = CELLA_ValueType::INT; // rowid 伪列：整数型物理行标识
                else
                    t = c ? dataTypeToValue(c->type) : CELLA_ValueType::UNKNOWN;
                break;
            }
            case CELLA_Expr::Kind::UNARY:
            {
                CELLA_ValueType ct;
                if (!resolveExpr(*e.child, scope, cat, errors, &ct))
                    return false;
                if (e.uop == CELLA_Expr::UnOp::IS_NULL ||
                    e.uop == CELLA_Expr::UnOp::IS_NOT_NULL)
                {
                    // 判空接受任意类型操作数（NULL 也是合法值），结果恒为 BOOL
                    t = CELLA_ValueType::BOOL;
                }
                else if (e.uop == CELLA_Expr::UnOp::NEG)
                {
                    if (!isNumericType(ct))
                    {
                        errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-309", e.line, e.col,
                                                         "一元操作符 '-' 不能应用于 " + valueTypeName(ct)));
                        return false;
                    }
                    t = ct;
                }
                else // NOT
                {
                    if (ct != CELLA_ValueType::BOOL)
                    {
                        errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-309", e.line, e.col,
                                                         "操作符 'NOT' 需要 BOOL 操作数，实际为 " +
                                                             valueTypeName(ct)));
                        return false;
                    }
                    t = CELLA_ValueType::BOOL;
                }
                break;
            }
            case CELLA_Expr::Kind::BINARY:
            {
                CELLA_ValueType lt, rt;
                if (!resolveExpr(*e.left, scope, cat, errors, &lt) ||
                    !resolveExpr(*e.right, scope, cat, errors, &rt))
                    return false;
                switch (e.bop)
                {
                case CELLA_Expr::BinOp::AND:
                case CELLA_Expr::BinOp::OR:
                    if (lt != CELLA_ValueType::BOOL || rt != CELLA_ValueType::BOOL)
                    {
                        errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-309", e.line, e.col,
                            "操作符 '" + binOpText(e.bop) + "' 需要 BOOL 操作数，实际为 " +
                                valueTypeName(lt) + " 与 " + valueTypeName(rt)));
                        return false;
                    }
                    t = CELLA_ValueType::BOOL;
                    break;
                case CELLA_Expr::BinOp::PLUS:
                case CELLA_Expr::BinOp::MINUS:
                case CELLA_Expr::BinOp::MUL:
                case CELLA_Expr::BinOp::DIV:
                    if (!isNumericType(lt) || !isNumericType(rt))
                    {
                        errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-309", e.line, e.col,
                            "操作符 '" + binOpText(e.bop) + "' 不能应用于 " + valueTypeName(lt) +
                                " 与 " + valueTypeName(rt)));
                        return false;
                    }
                    t = promoteNumeric(lt, rt);
                    break;
                case CELLA_Expr::BinOp::LIKE:
                case CELLA_Expr::BinOp::NOT_LIKE:
                {
                    // 通配匹配只在文本上定义；NULL 参与不报错（求值退化为 UNKNOWN）
                    const bool lok = isStringType(lt) || isDateTimeType(lt) ||
                                     lt == CELLA_ValueType::NULL_T;
                    const bool rok = isStringType(rt) || isDateTimeType(rt) ||
                                     rt == CELLA_ValueType::NULL_T;
                    if (!lok || !rok)
                    {
                        errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-325", e.line, e.col,
                            "操作符 '" + binOpText(e.bop) + "' 只支持文本操作数，实际为 " +
                                valueTypeName(lt) + " 与 " + valueTypeName(rt)));
                        return false;
                    }
                    t = CELLA_ValueType::BOOL;
                    break;
                }
                default: // 比较运算
                {
                    bool ok = (isNumericType(lt) && isNumericType(rt)) ||
                              (isStringType(lt) && isStringType(rt)) ||
                              (isDateTimeType(lt) && isDateTimeType(rt)) ||
                              (lt == CELLA_ValueType::BOOL && rt == CELLA_ValueType::BOOL) ||
                              lt == CELLA_ValueType::NULL_T || rt == CELLA_ValueType::NULL_T;
                    if (!ok)
                    {
                        errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-309", e.line, e.col,
                            "操作符 '" + binOpText(e.bop) + "' 不能应用于 " + valueTypeName(lt) +
                                " 与 " + valueTypeName(rt)));
                        return false;
                    }
                    t = CELLA_ValueType::BOOL;
                    break;
                }
                }
                break;
            }
            case CELLA_Expr::Kind::AGGREGATE:
            {
                const std::string fn = e.aggFunc.empty() ? "COUNT" : e.aggFunc;
                const bool is_count = (fn == "COUNT");
                // 通配参数只有 COUNT(*) 一种合法形式
                if (e.aggStar && !is_count)
                {
                    errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-323", e.line, e.col,
                        fn + "(*) 不合法：'*' 参数只对 COUNT 有意义"));
                    return false;
                }
                // COUNT(*) 不牵扯任何列；其余形式必须给出存在的列
                if (!e.aggStar &&
                    !resolveColumn(e.table, e.column, e.line, e.col, scope, cat, errors))
                    return false;
                CELLA_ValueType colType = CELLA_ValueType::UNKNOWN;
                if (!e.aggStar)
                {
                    const CELLA_Column *c = scopeFindColumn(scope, cat, e.table, e.column);
                    if (c != nullptr)
                        colType = dataTypeToValue(c->type);
                }
                if (is_count || e.aggStar)
                {
                    t = CELLA_ValueType::INT; // 计数结果恒为整数
                }
                else if (fn == "SUM" || fn == "AVG")
                {
                    if (colType != CELLA_ValueType::UNKNOWN && !isNumericType(colType))
                    {
                        errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-324", e.line, e.col,
                            fn + " 需要数值列，实际为 " + valueTypeName(colType) + "（列 " +
                                e.column + "）"));
                        return false;
                    }
                    // AVG 恒为 DOUBLE（即使整数列也可能除不尽）；SUM 整数列 → INT，否则 DOUBLE
                    t = (fn == "AVG") ? CELLA_ValueType::DOUBLE
                                      : ((colType == CELLA_ValueType::INT) ? CELLA_ValueType::INT
                                                                           : CELLA_ValueType::DOUBLE);
                }
                else // MIN / MAX：保持原列类型
                {
                    t = (colType == CELLA_ValueType::UNKNOWN) ? CELLA_ValueType::INT : colType;
                }
                break;
            }
            case CELLA_Expr::Kind::FUNCTION:
            {
                const CELLA_FuncSpec *spec = cella_findFunc(e.funcName);
                if (spec == nullptr)
                {
                    errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-330", e.line, e.col,
                                                     "未知函数 \"" + e.funcName +
                                                         "\"（可用：字符串 UPPER/LOWER/LENGTH/SUBSTR/"
                                                         "TRIM/LTRIM/RTRIM/REPLACE/CONCAT，数值 ABS/ROUND/"
                                                         "CEIL/FLOOR，日期 YEAR/MONTH/DAY/DATEDIFF/DATE_ADD/"
                                                         "DATE_SUB）"));
                    return false;
                }
                if (spec->windowOnly)
                {
                    errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-331", e.line, e.col,
                        spec->name + std::string(" 是窗口函数，必须带 OVER (...) 子句")));
                    return false;
                }
                const int n = static_cast<int>(e.args.size());
                if (n < spec->minArgs || (spec->maxArgs >= 0 && n > spec->maxArgs))
                {
                    std::string range = (spec->minArgs == spec->maxArgs)
                                            ? std::to_string(spec->minArgs)
                                            : (std::to_string(spec->minArgs) + "~" +
                                               std::to_string(spec->maxArgs));
                    errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-332", e.line, e.col,
                        std::string(spec->name) + " 实参个数为 " + range + "，实际 " +
                            std::to_string(n) + "（" + std::string(spec->note) + "）"));
                    return false;
                }
                for (const auto &a : e.args)
                {
                    if (!resolveExpr(*a, scope, cat, errors))
                        return false;
                }
                t = scalarFuncResultType(e.funcName);
                break;
            }
            case CELLA_Expr::Kind::WINDOW:
            {
                // 两种形态：
                //   ① 排名函数 ROW_NUMBER()/RANK()/DENSE_RANK() OVER (...)  —— 无实参
                //   ② 窗口聚合 SUM(x)/COUNT(*) OVER (...)                   —— 复用聚合列字段
                // 分区/排序列在这里就地校验（已经持有 scope，不必另设阶段）。
                for (const auto &cn : e.winPartition)
                {
                    if (!resolveColName(cn, scope, cat, errors))
                        return false;
                }
                for (const auto &cn : e.winOrder)
                {
                    if (!resolveColName(cn, scope, cat, errors))
                        return false;
                }
                for (const auto &a : e.args)
                {
                    if (!resolveExpr(*a, scope, cat, errors))
                        return false;
                }
                if (!e.aggFunc.empty())
                {
                    const std::string fn = e.aggFunc;
                    if (!e.aggStar &&
                        !resolveColumn(e.table, e.column, e.line, e.col, scope, cat, errors))
                        return false;
                    if (e.aggStar && fn != "COUNT")
                    {
                        errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-323", e.line, e.col,
                                                         fn + "(*) 不合法：只有 COUNT 接受 *"));
                        return false;
                    }
                    if (fn == "COUNT")
                        t = CELLA_ValueType::INT;
                    else if (fn == "AVG")
                        t = CELLA_ValueType::DOUBLE;
                    else
                        t = CELLA_ValueType::INT; // SUM/MIN/MAX 在此保守取 INT
                }
                else
                {
                    t = (e.funcName == "ROW_NUMBER" || e.funcName == "RANK" ||
                         e.funcName == "DENSE_RANK")
                            ? CELLA_ValueType::INT
                            : scalarFuncResultType(e.funcName);
                }
                break;
            }
            case CELLA_Expr::Kind::IN_LIST:
                // 常量列表：只校验被比较项与列表项，没有子查询体
                if (!resolveExpr(*e.left, scope, cat, errors))
                    return false;
                for (const auto &v : e.inList)
                {
                    if (!resolveExpr(*v, scope, cat, errors))
                        return false;
                }
                t = CELLA_ValueType::BOOL;
                break;
            case CELLA_Expr::Kind::IN_QUERY:
                if (!resolveExpr(*e.left, scope, cat, errors))
                    return false;
                if (!resolveSubqueryExpr(e, scope, cat, errors))
                    return false;
                t = CELLA_ValueType::BOOL;
                break;
            case CELLA_Expr::Kind::EXISTS_Q:
                if (!resolveSubqueryExpr(e, scope, cat, errors))
                    return false;
                t = CELLA_ValueType::BOOL;
                break;
            case CELLA_Expr::Kind::SCALAR_Q:
                if (!resolveSubqueryExpr(e, scope, cat, errors))
                    return false;
                t = subqueryScalarType(e);
                break;
            }
            if (outType)
                *outType = t;
            return true;
        }

        // 条件表达式检查：类型必须为 BOOL（limit/on/having/WHERE）
        bool checkBoolCondition(const CELLA_Expr &e, const std::vector<CELLA_ScopeEntry> &scope,
                                const CELLA_Catalog &cat, std::vector<CELLA_Error> &errors)
        {
            CELLA_ValueType t;
            if (!resolveExpr(e, scope, cat, errors, &t))
                return false;
            if (t != CELLA_ValueType::BOOL)
            {
                errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-310", e.line, e.col,
                                                 "条件表达式必须为 BOOL 类型，实际为 " + valueTypeName(t)));
                return false;
            }
            return true;
        }

        bool resolveColName(const CELLA_ColName &cn, const std::vector<CELLA_ScopeEntry> &scope,
                            const CELLA_Catalog &cat, std::vector<CELLA_Error> &errors)
        {
            return resolveColumn(cn.table, cn.column, cn.line, cn.col, scope, cat, errors);
        }

        std::string valueDisplay(const CELLA_Expr &e)
        {
            switch (e.lit)
            {
            case CELLA_LiteralKind::NUMBER:
                return e.text;
            case CELLA_LiteralKind::STRING:
                return "'" + e.text + "'";
            case CELLA_LiteralKind::DATE:
                return "'" + e.text + "'";
            case CELLA_LiteralKind::NULL_LIT:
                return "NULL";
            case CELLA_LiteralKind::BOOL_LIT:
                return e.boolVal ? "TRUE" : "FALSE";
            case CELLA_LiteralKind::DEFAULT_LIT:
                return "DEFAULT";
            }
            return "?";
        }

        std::string valueTypeLabel(const CELLA_Expr &e)
        {
            switch (e.lit)
            {
            case CELLA_LiteralKind::NUMBER:
                return "NUMBER";
            case CELLA_LiteralKind::STRING:
                return "STRING";
            case CELLA_LiteralKind::DATE:
                return "DATE";
            case CELLA_LiteralKind::NULL_LIT:
                return "NULL";
            case CELLA_LiteralKind::BOOL_LIT:
                return "BOOLEAN";
            case CELLA_LiteralKind::DEFAULT_LIT:
                return "DEFAULT";
            }
            return "?";
        }

        // 常量能否赋给目标列类型（简化规则见 README 实现决策）
        bool checkValueType(const CELLA_Expr &e, const CELLA_Column &col, std::vector<CELLA_Error> &errors)
        {
            bool ok = false;
            switch (e.lit)
            {
            case CELLA_LiteralKind::NUMBER:
                ok = col.type == CELLA_DataType::INT || col.type == CELLA_DataType::FLOAT ||
                     col.type == CELLA_DataType::DOUBLE;
                break;
            case CELLA_LiteralKind::STRING:
                ok = col.type == CELLA_DataType::CHAR || col.type == CELLA_DataType::VARCHAR ||
                     col.type == CELLA_DataType::TEXT || col.type == CELLA_DataType::TIME ||
                     col.type == CELLA_DataType::DATETIME;
                break;
            case CELLA_LiteralKind::DATE:
                ok = col.type == CELLA_DataType::DATE;
                break;
            case CELLA_LiteralKind::NULL_LIT:
                if (col.notNull)
                {
                    errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-307", e.line, e.col,
                                                     "不能向 NOT NULL 列 \"" + col.name + "\" 插入 NULL"));
                    return false;
                }
                return true;
            case CELLA_LiteralKind::BOOL_LIT:
                // BOOL 字面量只能赋给 BOOL 列（不隐式转 INT，避免 TRUE 被当作 1 的歧义）
                ok = col.type == CELLA_DataType::BOOL;
                break;
            case CELLA_LiteralKind::DEFAULT_LIT:
                // VALUES ( DEFAULT )：实际取值由执行层按列默认值决定，编译期不判类型
                return true;
            }
            if (!ok)
            {
                errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-306", e.line, e.col,
                                                 "常量 " + valueDisplay(e) + "(" + valueTypeLabel(e) +
                                                     ") 与列 " + col.name + "(" + cella_typeName(col.type) +
                                                     ") 类型不匹配"));
                return false;
            }
            return true;
        }

        std::string joinComma(const std::vector<std::string> &v)
        {
            std::string s;
            for (size_t i = 0; i < v.size(); i++)
            {
                if (i)
                    s += ", ";
                s += v[i];
            }
            return s;
        }

        // 索引列清单的解析：与 joinIndexColumnsText 互逆。
        // 编译器侧的 CELLA_Index 只保留持久化形态的 column 文本（无 columns 向量），
        // 因此改名/级联判定都要先拆开再比对。
        std::vector<std::string> splitIndexColumnsText(const std::string &joined)
        {
            std::vector<std::string> cols;
            std::string cur;
            for (char c : joined)
            {
                if (c == ',')
                {
                    if (!cur.empty())
                        cols.push_back(cur);
                    cur.clear();
                }
                else
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty())
                cols.push_back(cur);
            return cols;
        }

        // 索引列清单的持久化形态：**逗号分隔且不带空格**。
        // 必须与 cella_db 的 CatalogIndex::JoinedColumns() 逐字节一致 —— 编译器目录里
        // 的 index.column 会被写回 cella_index 系统表，格式不一致会让重启后解码错位。
        std::string joinIndexColumnsText(const std::vector<std::string> &v)
        {
            std::string s;
            for (size_t i = 0; i < v.size(); i++)
            {
                if (i)
                    s += ",";
                s += v[i];
            }
            return s;
        }

        // ---------------- 各语句 ----------------

        bool semCreateTable(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            if (cat.findTable(st.tableName))
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-302", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 已存在，重复定义"));
                return false;
            }
            std::set<std::string> seen;
            int primary_key_count = 0;
            for (const auto &cd : st.columns)
            {
                std::string key = cella_toUpper(cd.name);
                if (!seen.insert(key).second)
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-304", cd.line, cd.col,
                                                         "列 \"" + cd.name + "\" 在表 \"" + st.tableName +
                                                             "\" 中重复定义"));
                    return false;
                }
                if (isRowidName(cd.name))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-314", cd.line, cd.col,
                        "rowid 是每张表都有的只读伪列，不能用作列名（表 \"" + st.tableName + "\"）"));
                    return false;
                }
                if (cd.primaryKey)
                {
                    ++primary_key_count;
                }
            }

            // ── 主键仲裁：列级与表级互斥，二选一 ──────────────────
            if (!st.tablePrimaryKey.empty())
            {
                if (primary_key_count > 0)
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-313", st.tablePkLine, st.tablePkCol,
                        "表 \"" + st.tableName + "\" 同时定义了列级主键与表级主键（二选一）"));
                    return false;
                }
                std::set<std::string> pk_seen;
                // st 是解析器为本语句新建的 AST（编译期独享、无共享），标记表级主键需要
                // 回写列旗标 —— const 收敛在这一处。标记后，NOT NULL 隐含、目录编码、
                // 计划/语句打印全部复用列级主键的既有路径，无需任何特判。
                auto &mutable_st = const_cast<CELLA_Stmt &>(st);
                for (const auto &pkName : st.tablePrimaryKey)
                {
                    const std::string pkKey = cella_toUpper(pkName);
                    CELLA_ColumnDef *target = nullptr;
                    for (auto &cd : mutable_st.columns)
                    {
                        if (cella_toUpper(cd.name) == pkKey)
                        {
                            target = &cd;
                            break;
                        }
                    }
                    if (target == nullptr)
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-303", st.tablePkLine, st.tablePkCol,
                            "主键列 \"" + pkName + "\" 在表 \"" + st.tableName + "\" 中不存在"));
                        return false;
                    }
                    if (!pk_seen.insert(pkKey).second)
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-304", st.tablePkLine, st.tablePkCol,
                            "主键列 \"" + pkName + "\" 在 PRIMARY KEY 列表中重复"));
                        return false;
                    }
                    target->primaryKey = true; // 复合主键 = 多列被标记；隐含 NOT NULL 走既有路径
                }
            }
            else if (primary_key_count > 1)
            {
                // 多个列级 PRIMARY KEY：旧规则保留（复合主键请改用表级 PRIMARY KEY (a, b)）
                for (const auto &cd : st.columns)
                {
                    if (cd.primaryKey && primary_key_count > 1)
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-313", cd.line, cd.col,
                            "表 \"" + st.tableName + "\" 定义了多个列级主键（复合主键请用表级 PRIMARY KEY (a, b)）"));
                        return false;
                    }
                }
            }

            CELLA_Table table;
            table.name = st.tableName;
            for (const auto &cd : st.columns)
            {
                CELLA_Column c;
                c.name = cd.name;
                c.type = cd.type;
                c.notNull = cd.notNull || cd.primaryKey; // 主键隐含 NOT NULL
                c.primaryKey = cd.primaryKey;
                c.len = cd.hasLen ? cd.len : ((cd.type == CELLA_DataType::CHAR || cd.type == CELLA_DataType::VARCHAR) ? 255 : 0);
                table.columns.push_back(std::move(c));
            }
            cat.addTable(std::move(table));

            std::vector<std::string> names;
            for (const auto &cd : st.columns)
                names.push_back(cd.name);
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " CREATE TABLE " +
                                     st.tableName + "（列: " + joinComma(names) + "）");
            return true;
        }

        bool semDropTable(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            if (!cat.findTable(st.tableName))
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }
            cat.dropTable(st.tableName);
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " DROP TABLE " + st.tableName);
            return true;
        }

        bool semCreateIndex(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            const CELLA_Table *table = cat.findTable(st.tableName);
            if (!table)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }
            // 复合索引：逐列校验（indexColumns 为空时兜底解析 indexColumn，
            // 兼容手工构造 AST 的调用方）；同一列不允许出现两次。
            std::vector<std::string> cols = st.indexColumns;
            if (cols.empty() && !st.indexColumn.empty())
            {
                std::string cur;
                for (char c : st.indexColumn)
                {
                    if (c == ',')
                    {
                        cols.push_back(cur);
                        cur.clear();
                    }
                    else
                    {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty())
                {
                    cols.push_back(cur);
                }
            }
            if (cols.empty())
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                     "索引必须至少指定一列"));
                return false;
            }
            for (const std::string &c : cols)
            {
                if (!CELLA_Catalog::findColumn(*table, c))
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                         "列 \"" + c + "\" 不存在于表 \"" +
                                                             st.tableName + "\""));
                    return false;
                }
            }
            for (size_t i = 0; i < cols.size(); ++i)
            {
                for (size_t j = i + 1; j < cols.size(); ++j)
                {
                    if (cella_toUpper(cols[i]) == cella_toUpper(cols[j]))
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                            "索引列 \"" + cols[i] + "\" 重复出现在列清单中"));
                        return false;
                    }
                }
            }
            for (const auto &t : cat.allTables())
            {
                for (const auto &ex : t.second.indexes)
                {
                    if (cella_toUpper(ex.name) == cella_toUpper(st.indexName))
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-315", st.line, st.col,
                            "索引 \"" + st.indexName + "\" 已存在于表 \"" + t.second.name + "\""));
                        return false;
                    }
                }
            }
            CELLA_Index index;
            index.name = st.indexName;
            index.table = st.tableName;
            index.column = st.indexColumn;
            index.unique = st.unique;
            cat.addIndex(st.tableName, std::move(index));
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " CREATE " +
                                     std::string(st.unique ? "UNIQUE " : "") + "INDEX " + st.indexName +
                                     " ON " + st.tableName + "(" + st.indexColumn + ")");
            return true;
        }

        bool semDropIndex(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            const CELLA_Table *owner = nullptr;
            std::string ownerName;
            for (const auto &t : cat.allTables())
            {
                for (const auto &ex : t.second.indexes)
                {
                    if (cella_toUpper(ex.name) == cella_toUpper(st.indexName))
                    {
                        owner = &t.second;
                        ownerName = t.second.name;
                        break;
                    }
                }
                if (owner)
                    break;
            }
            if (!owner)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-316", st.line, st.col,
                                                     "索引 \"" + st.indexName + "\" 不存在"));
                return false;
            }
            const std::string tname = ownerName;
            cat.dropIndex(tname, st.indexName);
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " DROP INDEX " + st.indexName);
            return true;
        }

        // ---------------- ALTER TABLE / TRUNCATE TABLE（P5）----------------

        // 表上「已有主键」的判定：列旗标是权威（复合主键 = 多列带旗标）
        bool tableHasPrimaryKey(const CELLA_Table &t)
        {
            for (const auto &c : t.columns)
            {
                if (c.primaryKey)
                    return true;
            }
            return false;
        }

        std::string columnNamesOf(const CELLA_Table &t)
        {
            std::vector<std::string> names;
            for (const auto &c : t.columns)
                names.push_back(c.name);
            return joinComma(names);
        }

        bool semAlterTable(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            CELLA_Table *table = cat.findTable(st.tableName);
            if (!table)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }
            const std::string tname = table->name;
            const std::string okPrefix = "[语义] OK: 语句#" + std::to_string(idx) + " ALTER TABLE ";

            switch (st.alterAction)
            {
            case CELLA_Stmt::AlterAction::ADD_COLUMN:
            {
                const CELLA_ColumnDef &cd = st.newColumn;
                if (isRowidName(cd.name))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-314", cd.line, cd.col,
                        "rowid 是每张表都有的只读伪列，不能用作列名（表 \"" + tname + "\"）"));
                    return false;
                }
                if (CELLA_Catalog::findColumn(*table, cd.name))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-304", cd.line, cd.col,
                        "列 \"" + cd.name + "\" 在表 \"" + tname + "\" 中已存在"));
                    return false;
                }
                if (cd.primaryKey)
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-325", cd.line, cd.col,
                        "ADD COLUMN 不接受 PRIMARY KEY（新列无法为已有行补出主键值），"
                        "请改用 ALTER TABLE " + tname + " ADD PRIMARY KEY (列)"));
                    return false;
                }
                CELLA_Column c;
                c.name = cd.name;
                c.type = cd.type;
                c.notNull = cd.notNull;
                c.primaryKey = false;
                c.len = cd.hasLen ? cd.len
                                  : ((cd.type == CELLA_DataType::CHAR || cd.type == CELLA_DataType::VARCHAR)
                                         ? 255
                                         : 0);
                table->columns.push_back(std::move(c));
                res.okMessages.push_back(okPrefix + tname + " ADD COLUMN " + cd.name +
                                          "（列: " + columnNamesOf(*table) + "）");
                return true;
            }
            case CELLA_Stmt::AlterAction::DROP_COLUMN:
            {
                const CELLA_Column *col = CELLA_Catalog::findColumn(*table, st.alterColumnName);
                if (!col)
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                         "列 \"" + st.alterColumnName +
                                                             "\" 不存在于表 \"" + tname + "\""));
                    return false;
                }
                if (table->columns.size() <= 1)
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-323", st.line, st.col,
                        "不能删除表 \"" + tname + "\" 的最后一列（表至少要有一列）"));
                    return false;
                }
                if (col->primaryKey)
                {
                    // 删主键列会让主键残缺（复合主键尤其危险），故要求先显式解除主键
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-326", st.line, st.col,
                        "列 \"" + col->name + "\" 是主键列，不能直接删除；"
                        "请先执行 ALTER TABLE " + tname + " DROP PRIMARY KEY"));
                    return false;
                }
                const std::string target = cella_toUpper(col->name);
                // 级联：包含该列的索引一并删除（列没了，索引无从维护）
                std::vector<std::string> cascaded;
                for (auto it = table->indexes.begin(); it != table->indexes.end();)
                {
                    bool hit = false;
                    for (const std::string &ic : splitIndexColumnsText(it->column))
                    {
                        if (cella_toUpper(ic) == target)
                        {
                            hit = true;
                            break;
                        }
                    }
                    if (hit)
                    {
                        cascaded.push_back(it->name);
                        it = table->indexes.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                for (auto it = table->columns.begin(); it != table->columns.end(); ++it)
                {
                    if (cella_toUpper(it->name) == target)
                    {
                        table->columns.erase(it);
                        break;
                    }
                }
                std::string extra;
                if (!cascaded.empty())
                {
                    extra = "（级联删除索引: " + joinComma(cascaded) + "）";
                }
                res.okMessages.push_back(okPrefix + tname + " DROP COLUMN " + st.alterColumnName +
                                          "（列: " + columnNamesOf(*table) + "）" + extra);
                return true;
            }
            case CELLA_Stmt::AlterAction::RENAME_TABLE:
            {
                if (cella_toUpper(st.newName) == cella_toUpper(tname))
                {
                    // 改成同一个名字是空操作，放行（否则会误报「表已存在」）
                    res.okMessages.push_back(okPrefix + tname + " RENAME TO " + st.newName + "（无变化）");
                    return true;
                }
                if (cat.findTable(st.newName))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-302", st.line, st.col,
                        "表 \"" + st.newName + "\" 已存在，无法把 \"" + tname + "\" 改名为它"));
                    return false;
                }
                if (!cat.renameTable(tname, st.newName))
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-302", st.line, st.col,
                                                         "表改名失败: \"" + tname + "\" → \"" +
                                                             st.newName + "\""));
                    return false;
                }
                res.okMessages.push_back(okPrefix + tname + " RENAME TO " + st.newName);
                return true;
            }
            case CELLA_Stmt::AlterAction::RENAME_COLUMN:
            {
                CELLA_Column *col = nullptr;
                for (auto &c : table->columns)
                {
                    if (cella_toUpper(c.name) == cella_toUpper(st.alterColumnName))
                    {
                        col = &c;
                        break;
                    }
                }
                if (!col)
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                         "列 \"" + st.alterColumnName +
                                                             "\" 不存在于表 \"" + tname + "\""));
                    return false;
                }
                if (isRowidName(st.newName))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-314", st.line, st.col,
                        "rowid 是每张表都有的只读伪列，不能用作列名（表 \"" + tname + "\"）"));
                    return false;
                }
                if (cella_toUpper(st.newName) != cella_toUpper(col->name) &&
                    CELLA_Catalog::findColumn(*table, st.newName))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-304", st.line, st.col,
                        "列 \"" + st.newName + "\" 在表 \"" + tname + "\" 中已存在"));
                    return false;
                }
                const std::string oldName = col->name;
                col->name = st.newName;
                // 索引元数据里的列名同步改名（B+ 树键按**值**编码，不含列名 → 无需重建树）
                for (auto &ix : table->indexes)
                {
                    std::vector<std::string> cols = splitIndexColumnsText(ix.column);
                    for (auto &ic : cols)
                    {
                        if (cella_toUpper(ic) == cella_toUpper(oldName))
                            ic = st.newName;
                    }
                    ix.column = joinIndexColumnsText(cols);
                }
                res.okMessages.push_back(okPrefix + tname + " RENAME COLUMN " + oldName + " TO " +
                                          st.newName);
                return true;
            }
            case CELLA_Stmt::AlterAction::ADD_PRIMARY_KEY:
            {
                if (tableHasPrimaryKey(*table))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-327", st.line, st.col,
                        "表 \"" + tname + "\" 已有主键；请先 ALTER TABLE " + tname +
                            " DROP PRIMARY KEY"));
                    return false;
                }
                if (st.pkColumns.empty())
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                         "ADD PRIMARY KEY 至少需要一列"));
                    return false;
                }
                std::set<std::string> seen;
                for (const std::string &cn : st.pkColumns)
                {
                    CELLA_Column *col = nullptr;
                    for (auto &c : table->columns)
                    {
                        if (cella_toUpper(c.name) == cella_toUpper(cn))
                        {
                            col = &c;
                            break;
                        }
                    }
                    if (!col)
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                            "主键列 \"" + cn + "\" 不存在于表 \"" + tname + "\""));
                        return false;
                    }
                    if (!seen.insert(cella_toUpper(cn)).second)
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-304", st.line, st.col,
                            "主键列 \"" + cn + "\" 在列表中重复"));
                        return false;
                    }
                    // 先记下待置位，全部校验通过后统一生效（避免部分成功）
                    (void)col;
                }
                for (const std::string &cn : st.pkColumns)
                {
                    for (auto &c : table->columns)
                    {
                        if (cella_toUpper(c.name) == cella_toUpper(cn))
                        {
                            c.primaryKey = true;
                            c.notNull = true; // 主键隐含非空（与 CREATE TABLE 一致）
                        }
                    }
                }
                res.okMessages.push_back(okPrefix + tname + " ADD PRIMARY KEY (" +
                                          joinComma(st.pkColumns) + ")");
                return true;
            }
            case CELLA_Stmt::AlterAction::DROP_PRIMARY_KEY:
            {
                if (!tableHasPrimaryKey(*table))
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-328", st.line, st.col,
                                                         "表 \"" + tname + "\" 没有主键"));
                    return false;
                }
                for (auto &c : table->columns)
                {
                    // 只解除主键约束；NOT NULL 保留（MySQL 同样如此，避免「删主键顺手放宽空值」的意外）
                    c.primaryKey = false;
                }
                res.okMessages.push_back(okPrefix + tname + " DROP PRIMARY KEY");
                return true;
            }
            }
            res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-399", st.line, st.col,
                                                 "未知的 ALTER TABLE 动作"));
            return false;
        }

        bool semTruncateTable(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx,
                              CELLA_SemanticResult &res)
        {
            const CELLA_Table *table = cat.findTable(st.tableName);
            if (!table)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }
            // TRUNCATE 不动结构，故目录无需变化（索引定义与列定义都保留）
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " TRUNCATE TABLE " +
                                     table->name);
            return true;
        }

        bool semInsert(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            const CELLA_Table *table = cat.findTable(st.tableName);
            if (!table)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }

            // 目标列：有列清单按列清单，否则按建表列序取全部列
            std::vector<const CELLA_Column *> targets;
            if (!st.insertColumns.empty())
            {
                for (const auto &name : st.insertColumns)
                {
                    const CELLA_Column *c = CELLA_Catalog::findColumn(*table, name);
                    if (!c)
                    {
                        res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                             "列 \"" + name + "\" 不存在于表 \"" +
                                                                 st.tableName + "\""));
                        return false;
                    }
                    targets.push_back(c);
                }
            }
            else
            {
                for (const auto &c : table->columns)
                    targets.push_back(&c);
            }

            for (const auto &row : st.rows)
            {
                if (row.size() != targets.size())
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-305", st.line, st.col,
                        "INSERT 值个数(" + std::to_string(row.size()) + ") 与列个数(" +
                            std::to_string(targets.size()) + ") 不一致"));
                    return false;
                }
                for (size_t i = 0; i < row.size(); i++)
                {
                    if (!checkValueType(*row[i], *targets[i], res.errors))
                        return false;
                }
            }
            // 记录实际目标列（省略列清单时展开为全部列），供计划阶段使用
            std::vector<std::string> names;
            for (const CELLA_Column *c : targets)
                names.push_back(c->name);
            if (idx > 0 && static_cast<size_t>(idx) <= res.insertColumns.size())
                res.insertColumns[idx - 1] = std::move(names);
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " INSERT INTO " +
                                     st.tableName);
            return true;
        }

        bool semDelete(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            const CELLA_Table *table = cat.findTable(st.tableName);
            if (!table)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }
            if (st.where)
            {
                std::vector<CELLA_ScopeEntry> scope{{st.tableName, ""}};
                if (!checkBoolCondition(*st.where, scope, cat, res.errors))
                    return false;
            }
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " DELETE " +
                                     st.tableName);
            return true;
        }

        bool semUpdate(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            const CELLA_Table *table = cat.findTable(st.tableName);
            if (!table)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.line, st.col,
                                                     "表 \"" + st.tableName + "\" 不存在"));
                return false;
            }
            std::vector<CELLA_ScopeEntry> scope{{st.tableName, ""}};
            for (const auto &s : st.sets)
            {
                const CELLA_Column *col = CELLA_Catalog::findColumn(*table, s.first);
                if (!col)
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-303", st.line, st.col,
                                                         "列 \"" + s.first + "\" 不存在于表 \"" +
                                                             st.tableName + "\""));
                    return false;
                }
                // 表达式内部类型检查（列存在性 + 运算类型规则）
                CELLA_ValueType et;
                if (!resolveExpr(*s.second, scope, cat, res.errors, &et))
                    return false;
                if (s.second->kind == CELLA_Expr::Kind::LITERAL)
                {
                    if (!checkValueType(*s.second, *col, res.errors))
                        return false;
                }
                else
                {
                    CELLA_ValueType ct = dataTypeToValue(col->type);
                    bool ok = isNumericType(et) ? isNumericType(ct)
                                                : (isStringType(et) ? isStringType(ct) : et == ct);
                    if (!ok)
                    {
                        res.errors.push_back(cella_makeError(
                            CELLA_Phase::SEM, "SEM-306", s.second->line, s.second->col,
                            "表达式(" + valueTypeName(et) + ") 与列 " + col->name + "(" +
                                cella_typeName(col->type) + ") 类型不匹配"));
                        return false;
                    }
                }
            }
            if (st.where)
            {
                if (!checkBoolCondition(*st.where, scope, cat, res.errors))
                    return false;
            }
            res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " UPDATE " + st.tableName);
            return true;
        }

        // 收集表达式树里出现的聚合调用（P4 只有 COUNT）
        void collectAggregates(const CELLA_Expr &e, std::vector<const CELLA_Expr *> &out)
        {
            if (e.kind == CELLA_Expr::Kind::AGGREGATE)
                out.push_back(&e);
            if (e.left)
                collectAggregates(*e.left, out);
            if (e.right)
                collectAggregates(*e.right, out);
            if (e.child)
                collectAggregates(*e.child, out);
        }

        bool exprHasAggregate(const CELLA_Expr &e)
        {
            std::vector<const CELLA_Expr *> v;
            collectAggregates(e, v);
            return !v.empty();
        }

        // 校验一条 SELECT 的聚合语义（任务书 P4）：
        //  1. 分组键不得是聚合表达式；
        //  2. HAVING 里不得出现聚合（本阶段不支持）；
        //  3. 出现聚合时（或存在 grouped），投影项要么是聚合、要么是分组键；
        //  4. 没有分组键却有聚合 → 全表聚合成一行（COUNT 天然允许）。
        bool checkAggregateSemantics(const CELLA_Stmt &st, std::vector<CELLA_Error> &errors)
        {
            if (st.having)
            {
                std::vector<const CELLA_Expr *> aggs;
                collectAggregates(*st.having, aggs);
                if (!aggs.empty())
                {
                    errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-320", aggs[0]->line, aggs[0]->col,
                        "HAVING 中暂不支持聚合函数"));
                }
            }
            if (st.star)
                return true;
            // 是否存在聚合或分组
            bool hasAgg = false;
            std::vector<const CELLA_Expr *> aggList;
            for (const auto &si : st.selectItems)
            {
                collectAggregates(*si.expr, aggList);
            }
            hasAgg = !aggList.empty();
            if (!hasAgg && st.grouped.empty())
                return true;
            // 逐项检查：聚合项放行；非聚合项必须恰好等于某个分组键
            for (const auto &si : st.selectItems)
            {
                if (exprHasAggregate(*si.expr))
                    continue;
                // 非聚合项：必须是纯列引用且命中分组键
                if (si.expr->kind != CELLA_Expr::Kind::COLUMN_REF)
                {
                    errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-321", si.expr->line, si.expr->col,
                        "SELECT 项既不是聚合函数也不是分组键（出现聚合/分组时只允许这两类）"));
                    return false;
                }
                bool hit = false;
                for (const auto &cn : st.grouped)
                {
                    if (cella_toUpper(cn.column) == cella_toUpper(si.expr->column) &&
                        (cn.table.empty() || si.expr->table.empty() ||
                         cella_toUpper(cn.table) == cella_toUpper(si.expr->table)))
                    {
                        hit = true;
                        break;
                    }
                }
                if (!hit)
                {
                    errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-322", si.expr->line, si.expr->col,
                        "列 \"" + si.expr->column + "\" 既不在 GROUP BY 中也不是聚合函数"));
                    return false;
                }
            }
            return true;
        }

        bool semGet(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res,
                    bool topLevel)
        {
            std::vector<CELLA_ScopeEntry> scope;
            const CELLA_Table *t = cat.findTable(st.from.name);
            if (!t)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", st.from.line, st.from.col,
                                                     "表 \"" + st.from.name + "\" 不存在"));
                return false;
            }
            scope.push_back(CELLA_ScopeEntry{st.from.name, st.from.alias});
            for (const auto &jc : st.joins)
            {
                const CELLA_Table *jt = cat.findTable(jc.ref.name);
                if (!jt)
                {
                    res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-301", jc.ref.line, jc.ref.col,
                                                         "表 \"" + jc.ref.name + "\" 不存在"));
                    return false;
                }
                scope.push_back(CELLA_ScopeEntry{jc.ref.name, jc.ref.alias});
            }
            if (!st.star)
            {
                for (const auto &si : st.selectItems)
                {
                    if (!resolveExpr(*si.expr, scope, cat, res.errors))
                        return false;
                }
            }
            if (st.limit && !checkBoolCondition(*st.limit, scope, cat, res.errors))
                return false;
            for (const auto &jc : st.joins)
            {
                if (!checkBoolCondition(*jc.on, scope, cat, res.errors))
                    return false;
            }
            if (st.having && !checkBoolCondition(*st.having, scope, cat, res.errors))
                return false;
            for (const auto &cn : st.grouped)
            {
                if (!resolveColName(cn, scope, cat, res.errors))
                    return false;
            }
            if (!checkAggregateSemantics(st, res.errors))
                return false;
            for (const auto &oi : st.ordered)
            {
                if (!resolveColName(oi.col, scope, cat, res.errors))
                    return false;
            }
            if (st.unionQuery)
            {
                if (!semGet(*st.unionQuery, cat, idx, res, false))
                    return false;
            }
            // 分页参数必须为正整数（页码 1 起，每页行数 ≥ 1）
            if (st.hasPage && (st.pageNo < 1 || st.pageSize < 1))
            {
                res.errors.push_back(cella_makeError(
                    CELLA_Phase::SEM, "SEM-311", st.line, st.col,
                    "分页参数必须为正整数（页码 " + std::to_string(st.pageNo) + "，每页行数 " +
                        std::to_string(st.pageSize) + "）"));
                return false;
            }
            // 分页与 among 冲突校验：among 先限定总行数，page 的起始行不得超出该范围
            if (st.hasPage && st.among >= 0)
            {
                long long offset = (st.pageNo - 1) * st.pageSize;
                if (offset >= st.among)
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-312", st.line, st.col,
                        "分页与 among 冲突：页码 " + std::to_string(st.pageNo) + "（起始行 " +
                            std::to_string(offset + 1) + "）超出 among 限定的 " + std::to_string(st.among) +
                            " 行"));
                    return false;
                }
            }
            if (topLevel)
            {
                res.okMessages.push_back("[语义] OK: 语句#" + std::to_string(idx) + " get in " +
                                         st.from.name);
            }
            return true;
        }

        // 从定义查询推导视图的输出列。
        // 编译器侧把视图登记成一张普通表（列即推导结果），这样 semGet 完全复用，
        // 不必为视图单独写一套列解析；执行期再按视图定义展开。
        bool deriveViewColumns(const CELLA_Stmt &q, const CELLA_Catalog &cat,
                               std::vector<CELLA_Column> &outCols)
        {
            const CELLA_Table *base = cat.findTable(q.from.name);
            if (!base)
                return false;
            if (q.star)
            {
                outCols = base->columns;
                return true;
            }
            for (const auto &si : q.selectItems)
            {
                if (!si.expr)
                    continue;
                const CELLA_Expr &e = *si.expr;
                CELLA_Column c;
                if (!si.alias.empty())
                {
                    c.name = si.alias;
                }
                else if (e.kind == CELLA_Expr::Kind::COLUMN_REF)
                {
                    c.name = e.column;
                }
                else if (e.kind == CELLA_Expr::Kind::AGGREGATE)
                {
                    c.name = e.aggFunc;
                }
                else
                {
                    c.name = e.funcName.empty() ? std::string("expr") : e.funcName;
                }
                if (e.kind == CELLA_Expr::Kind::COLUMN_REF)
                {
                    if (const CELLA_Column *src = CELLA_Catalog::findColumn(*base, e.column))
                        c.type = src->type;
                }
                else if (e.kind == CELLA_Expr::Kind::AGGREGATE)
                {
                    c.type = (e.aggFunc == "AVG") ? CELLA_DataType::DOUBLE : CELLA_DataType::INT;
                }
                else if (e.kind == CELLA_Expr::Kind::WINDOW)
                {
                    c.type = (e.funcName == "ROW_NUMBER" || e.funcName == "RANK" ||
                              e.funcName == "DENSE_RANK" || e.aggFunc == "COUNT")
                                 ? CELLA_DataType::INT
                                 : CELLA_DataType::DOUBLE;
                }
                else
                {
                    c.type = CELLA_DataType::INT;
                }
                outCols.push_back(c);
            }
            return true;
        }

        // CREATE VIEW name AS <get>
        bool semCreateView(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            if (!st.viewQuery)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-350", st.line, st.col,
                                                     "CREATE VIEW 缺少定义查询"));
                return false;
            }
            // 定义查询在目录副本上校验：视图定义不得产生目录副作用。
            CELLA_Catalog sub = cat;
            CELLA_SemanticResult subRes;
            if (!analyzeStmt(*st.viewQuery, sub, idx, subRes))
            {
                for (const auto &e : subRes.errors)
                    res.errors.push_back(e);
                res.errors.push_back(cella_makeError(
                    CELLA_Phase::SEM, "SEM-351", st.line, st.col,
                    "视图 \"" + st.viewName + "\" 的定义查询未通过语义检查"));
                return false;
            }
            if (cat.findTable(st.viewName) != nullptr)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-352", st.line, st.col,
                                                     "视图名 \"" + st.viewName +
                                                         "\" 与已存在的表/视图重名"));
                return false;
            }
            CELLA_Table vt;
            vt.name = st.viewName;
            if (!deriveViewColumns(*st.viewQuery, cat, vt.columns))
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-353", st.line, st.col,
                                                     "无法推导视图 \"" + st.viewName + "\" 的输出列"));
                return false;
            }
            if (!cat.addTable(std::move(vt)))
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-352", st.line, st.col,
                                                     "视图 \"" + st.viewName + "\" 创建失败"));
                return false;
            }
            return true;
        }

        // DROP VIEW name
        bool semDropView(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            (void)idx;
            if (cat.findTable(st.viewName) == nullptr)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-354", st.line, st.col,
                                                     "视图 \"" + st.viewName + "\" 不存在"));
                return false;
            }
            cat.dropTable(st.viewName);
            return true;
        }

        // WITH n1 AS ( get ... ) [, ...] <主语句>
        // CTE 只在语句作用域内可见，故全部登记在目录副本上，不写回外层目录。
        bool semWith(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            CELLA_Catalog sub = cat;
            for (size_t i = 0; i < st.cteNames.size(); i++)
            {
                CELLA_SemanticResult r;
                if (!analyzeStmt(*st.cteQueries[i], sub, idx, r))
                {
                    for (const auto &e : r.errors)
                        res.errors.push_back(e);
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-355", st.line, st.col,
                        "CTE \"" + st.cteNames[i] + "\" 的定义查询未通过语义检查"));
                    return false;
                }
                CELLA_Table vt;
                vt.name = st.cteNames[i];
                if (!deriveViewColumns(*st.cteQueries[i], sub, vt.columns) || !sub.addTable(std::move(vt)))
                {
                    res.errors.push_back(cella_makeError(
                        CELLA_Phase::SEM, "SEM-356", st.line, st.col,
                        "CTE \"" + st.cteNames[i] + "\" 登记失败（重名或无法推导列）"));
                    return false;
                }
            }
            if (!st.cteMain)
            {
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-357", st.line, st.col,
                                                     "WITH 缺少主语句"));
                return false;
            }
            CELLA_SemanticResult r2;
            if (!analyzeStmt(*st.cteMain, sub, idx, r2))
            {
                for (const auto &e : r2.errors)
                    res.errors.push_back(e);
                res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-358", st.line, st.col,
                                                     "WITH 主语句未通过语义检查"));
                return false;
            }
            return true;
        }

        bool analyzeStmt(const CELLA_Stmt &st, CELLA_Catalog &cat, int idx, CELLA_SemanticResult &res)
        {
            switch (st.kind)
            {
            case CELLA_Stmt::Kind::CREATE_TABLE:
                return semCreateTable(st, cat, idx, res);
            case CELLA_Stmt::Kind::DROP_TABLE:
                return semDropTable(st, cat, idx, res);
            case CELLA_Stmt::Kind::CREATE_INDEX:
                return semCreateIndex(st, cat, idx, res);
            case CELLA_Stmt::Kind::DROP_INDEX:
                return semDropIndex(st, cat, idx, res);
            case CELLA_Stmt::Kind::ALTER_TABLE:
                return semAlterTable(st, cat, idx, res);
            case CELLA_Stmt::Kind::TRUNCATE_TABLE:
                return semTruncateTable(st, cat, idx, res);
            case CELLA_Stmt::Kind::INSERT:
                return semInsert(st, cat, idx, res);
            case CELLA_Stmt::Kind::DELETE:
                return semDelete(st, cat, idx, res);
            case CELLA_Stmt::Kind::UPDATE:
                return semUpdate(st, cat, idx, res);
            case CELLA_Stmt::Kind::GET:
                return semGet(st, cat, idx, res, true);
            case CELLA_Stmt::Kind::CREATE_VIEW:
                return semCreateView(st, cat, idx, res);
            case CELLA_Stmt::Kind::DROP_VIEW:
                return semDropView(st, cat, idx, res);
            case CELLA_Stmt::Kind::WITH:
                return semWith(st, cat, idx, res);
            }
            res.errors.push_back(cella_makeError(CELLA_Phase::SEM, "SEM-399", st.line, st.col,
                                                 "未知语句类型"));
            return false;
        }

    } // namespace

    // 公共包装：语义内部与执行期（视图/CTE 登记）共用同一套列推导规则。
    bool cella_deriveQueryColumns(const CELLA_Stmt &query, const CELLA_Catalog &catalog,
                                  std::vector<CELLA_Column> *outCols)
    {
        if (outCols == nullptr)
            return false;
        outCols->clear();
        return deriveViewColumns(query, catalog, *outCols);
    }

    CELLA_SemanticResult cella_analyze(const CELLA_Program &program, CELLA_Catalog &catalog)
    {
        CELLA_SemanticResult result;
        result.stmtOk.assign(program.statements.size(), false);
        result.insertColumns.assign(program.statements.size(), std::vector<std::string>());
        for (size_t i = 0; i < program.statements.size(); i++)
        {
            result.stmtOk[i] = analyzeStmt(*program.statements[i], catalog, static_cast<int>(i + 1),
                                           result);
        }
        return result;
    }

} // namespace cella
