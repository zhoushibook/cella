// cella_common.h —— 公共定义：统一错误结构与字符串工具。
// 约定：各阶段错误收集到 std::vector<CELLA_Error> 后由 CLI 统一输出，不使用异常。
#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace cella
{

    // 错误阶段（决定诊断行前缀）
    enum class CELLA_Phase
    {
        LEX,
        SYN,
        SEM,
        PLN
    };

    inline const char *cella_phaseText(CELLA_Phase p)
    {
        switch (p)
        {
        case CELLA_Phase::LEX:
            return "词法";
        case CELLA_Phase::SYN:
            return "语法";
        case CELLA_Phase::SEM:
            return "语义";
        case CELLA_Phase::PLN:
            return "计划";
        }
        return "?";
    }

    // 统一错误：阶段 + 错误码 + 位置(行:列，1 起) + 说明
    struct CELLA_Error
    {
        CELLA_Phase phase = CELLA_Phase::LEX;
        std::string code;
        int line = 0;
        int col = 0;
        std::string message;
    };

    // 按任务书第 4 节契约格式化为一行
    inline std::string cella_errorText(const CELLA_Error &e)
    {
        std::ostringstream os;
        os << "[" << cella_phaseText(e.phase) << "] " << e.code << " @"
           << e.line << ":" << e.col << " " << e.message;
        return os.str();
    }

    inline CELLA_Error cella_makeError(CELLA_Phase phase, const std::string &code, int line, int col,
                                       const std::string &message)
    {
        CELLA_Error e;
        e.phase = phase;
        e.code = code;
        e.line = line;
        e.col = col;
        e.message = message;
        return e;
    }

    // 大写化（用于关键字与目录的大小写不敏感比较）
    inline std::string cella_toUpper(const std::string &s)
    {
        std::string r = s;
        for (auto &c : r)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }

    // 是否形如 YYYY-MM-DD（日期字面量识别）
    inline bool cella_isDateText(const std::string &s)
    {
        if (s.size() != 10)
            return false;
        if (s[4] != '-' || s[7] != '-')
            return false;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (i == 4 || i == 7)
                continue;
            if (!std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
        }
        return true;
    }

    // ── 标量 / 窗口函数规格表 ────────────────────────────────────
    // 语义阶段（cella_sql）与执行阶段（cella_db）共用同一份定义，
    // 避免「编译器放行、执行器不认」或反过来的漂移。
    // 约定：name 为大写；minArgs/maxArgs 为实参个数区间（-1 表示不限）。
    struct CELLA_FuncSpec
    {
        const char *name;
        int minArgs;
        int maxArgs;
        bool windowOnly; // 仅可用于窗口上下文（必须有 OVER 子句）
        const char *note;
    };

    inline const CELLA_FuncSpec *cella_findFunc(const std::string &upperName)
    {
        static const CELLA_FuncSpec kFuncs[] = {
            // 字符串函数
            {"UPPER", 1, 1, false, "转为大写"},
            {"LOWER", 1, 1, false, "转为小写"},
            {"LENGTH", 1, 1, false, "字符数"},
            {"CHAR_LENGTH", 1, 1, false, "字符数（LENGTH 别名）"},
            {"SUBSTR", 2, 3, false, "取子串：SUBSTR(s, start[, len])"},
            {"SUBSTRING", 2, 3, false, "SUBSTR 别名"},
            {"TRIM", 1, 1, false, "去掉首尾空白"},
            {"LTRIM", 1, 1, false, "去掉左侧空白"},
            {"RTRIM", 1, 1, false, "去掉右侧空白"},
            {"REPLACE", 3, 3, false, "替换：REPLACE(s, from, to)"},
            {"CONCAT", 1, 8, false, "拼接（NULL 视作空串）"},
            // 数值函数
            {"ABS", 1, 1, false, "绝对值"},
            {"ROUND", 1, 2, false, "四舍五入：ROUND(x[, n])"},
            {"CEIL", 1, 1, false, "向上取整"},
            {"FLOOR", 1, 1, false, "向下取整"},
            // 日期函数（日期以 YYYY-MM-DD 文本存储，函数内部换算为天数运算）
            {"YEAR", 1, 1, false, "取年份"},
            {"MONTH", 1, 1, false, "取月份"},
            {"DAY", 1, 1, false, "取日"},
            {"DATEDIFF", 2, 2, false, "相差天数：DATEDIFF(a, b) = a - b"},
            {"DATE_ADD", 2, 2, false, "日期加减天数：DATE_ADD(d, n)"},
            {"DATE_SUB", 2, 2, false, "日期减天数：DATE_SUB(d, n)"},
            // 窗口函数（必须有 OVER 子句）
            {"ROW_NUMBER", 0, 0, true, "分区内行号（1 起）"},
            {"RANK", 0, 0, true, "分区内排名（并列跳号）"},
            {"DENSE_RANK", 0, 0, true, "分区内排名（并列不跳号）"},
        };
        for (const auto &f : kFuncs)
        {
            if (upperName == f.name)
                return &f;
        }
        return nullptr;
    }

    // 窗口聚合函数（OVER 子句内可用的聚合名）
    inline bool cella_isWindowAggName(const std::string &upperName)
    {
        return upperName == "COUNT" || upperName == "SUM" || upperName == "AVG" ||
               upperName == "MIN" || upperName == "MAX";
    }

} // namespace cella
