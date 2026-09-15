// cella_catalog.h —— 内存目录：表/列结构与大小写不敏感的表查询（任务书 3.3 节）。
// 一次进程内累积、不重置；进程结束后不保留。CREATE TABLE / DROP TABLE 会改变它。
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cella_common.h"

namespace cella
{

    enum class CELLA_DataType
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
        BOOL
    };

    inline std::string cella_typeName(CELLA_DataType t)
    {
        switch (t)
        {
        case CELLA_DataType::INT:
            return "INT";
        case CELLA_DataType::FLOAT:
            return "FLOAT";
        case CELLA_DataType::DOUBLE:
            return "DOUBLE";
        case CELLA_DataType::CHAR:
            return "CHAR";
        case CELLA_DataType::VARCHAR:
            return "VARCHAR";
        case CELLA_DataType::TEXT:
            return "TEXT";
        case CELLA_DataType::DATE:
            return "DATE";
        case CELLA_DataType::TIME:
            return "TIME";
        case CELLA_DataType::DATETIME:
            return "DATETIME";
        case CELLA_DataType::BOOL:
            return "BOOL";
        }
        return "?";
    }

    struct CELLA_Column
    {
        std::string name; // 原始拼写
        CELLA_DataType type = CELLA_DataType::INT;
        int len = 0; // CHAR/VARCHAR 长度（非字符类型为 0）
        bool notNull = false;
        // 主键列（P5）。编译器侧只用于「至多一个主键 / 已有主键」这类判断，
        // 不参与打印，因此不影响任何既有输出。权威来源仍是 cella_catalog 的列编码。
        bool primaryKey = false;
    };

    // 二级索引（P1.2）：语义层的轻量登记，真实元数据落在 cella_db 的 cella_index 系统表
    struct CELLA_Index
    {
        std::string name;   // 索引名（原始拼写）
        std::string table;  // 所属表
        std::string column; // 索引列（单列 = 列名；复合 = "a,b" 逗号拼接）
        bool unique = false;
    };

    struct CELLA_Table
    {
        std::string name; // 原始拼写
        std::vector<CELLA_Column> columns;
        std::vector<CELLA_Index> indexes;
    };

    // 目录：表名/列名比较不区分大小写（内部以大写为键）
    struct CELLA_Catalog
    {
        std::map<std::string, CELLA_Table> tables;

        const CELLA_Table *findTable(const std::string &name) const
        {
            auto it = tables.find(cella_toUpper(name));
            return it == tables.end() ? nullptr : &it->second;
        }

        CELLA_Table *findTable(const std::string &name)
        {
            auto it = tables.find(cella_toUpper(name));
            return it == tables.end() ? nullptr : &it->second;
        }

        static const CELLA_Column *findColumn(const CELLA_Table &table, const std::string &name)
        {
            std::string key = cella_toUpper(name);
            for (const auto &c : table.columns)
            {
                if (cella_toUpper(c.name) == key)
                    return &c;
            }
            return nullptr;
        }

        bool addTable(CELLA_Table table)
        {
            std::string key = cella_toUpper(table.name);
            if (tables.count(key) != 0)
                return false;
            tables.emplace(std::move(key), std::move(table));
            return true;
        }

        bool dropTable(const std::string &name) { return tables.erase(cella_toUpper(name)) > 0; }

        // 表改名（P5）：键与表内 name 一起改。新名已存在返回 false。
        bool renameTable(const std::string &oldName, const std::string &newName)
        {
            const std::string oldKey = cella_toUpper(oldName);
            const std::string newKey = cella_toUpper(newName);
            auto it = tables.find(oldKey);
            if (it == tables.end() || tables.count(newKey) != 0)
                return false;
            CELLA_Table moved = std::move(it->second);
            tables.erase(it);
            moved.name = newName;
            // 索引元数据里的 table 也指向新名（语义层靠它做索引归属判断）
            for (auto &ix : moved.indexes)
                ix.table = newName;
            tables.emplace(newKey, std::move(moved));
            return true;
        }

        // 只读遍历（语义层用于全局索引名查重 / 反查所属表）
        const std::map<std::string, CELLA_Table> &allTables() const { return tables; }

        bool addIndex(const std::string &table, CELLA_Index index)
        {
            CELLA_Table *t = findTable(table);
            if (!t)
                return false;
            std::string key = cella_toUpper(index.name);
            for (const auto &ex : t->indexes)
            {
                if (cella_toUpper(ex.name) == key)
                    return false;
            }
            t->indexes.push_back(std::move(index));
            return true;
        }

        bool dropIndex(const std::string &table, const std::string &index)
        {
            CELLA_Table *t = findTable(table);
            if (!t)
                return false;
            std::string key = cella_toUpper(index);
            for (auto it = t->indexes.begin(); it != t->indexes.end(); ++it)
            {
                if (cella_toUpper(it->name) == key)
                {
                    t->indexes.erase(it);
                    return true;
                }
            }
            return false;
        }
    };

} // namespace cella
