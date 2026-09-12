// value_bridge.h —— SQL 侧类型/字面量 ⇄ 存储侧 Value 的双向桥。
//
// 这是整合层最基础的一层适配，回答三个问题：
//   1) 编译器的 SQL 数据类型，落到存储里用什么物理类型？
//   2) 编译器 AST 里的字面量表达式，怎么变成一行记录里的单元格值？
//   3) 存储读回来的 Value，怎么比较、怎么渲染成给人看的结果？
//
// 物理类型映射（受存储层序列化器能力约束，见 slotted_record_serializer.cpp）：
//   INT            → kInt32
//   FLOAT          → kFloat
//   DOUBLE         → kDouble
//   CHAR/VARCHAR/TEXT → kVarchar
//   DATE/TIME/DATETIME → kVarchar
//     （存储层未实现 kDate 物理类型；ISO 日期/时间文本的字典序与时间序一致，
//      比较语义因此正确。这是已知的有意简化，记录于 docs/INTEGRATION.md。）
#pragma once

#include <cstdint>
#include <string>

#include "cella/cella_ast.h"
#include "cella/cella_catalog.h"
#include "cella/db/common/db_status.h"
#include "cella/storage/common/record.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/types.h"
#include "cella/storage/common/value.h"

namespace cella::db
{

    struct CatalogTable;

    using storage::Record;
    using storage::Schema;
    using storage::Value;
    using storage::ValueType;

    // ── 类型桥 ────────────────────────────────────────────────
    ValueType ToStorageType(cella::CELLA_DataType t);

    // 列声明的最大长度（CHAR/VARCHAR：显式长度，未声明默认 255；TEXT：0 = 不限）
    uint16_t StorageMaxLen(cella::CELLA_DataType t, int declared_len, bool has_len);

    // 目录元数据 → 存储 schema（表结构在存储层的表示）
    Schema ToStorageSchema(const cella::CELLA_Table &table);
    Schema ToStorageSchema(const CatalogTable &table);

    // ── 字面量桥 ──────────────────────────────────────────────
    // 数字字面量文本是否表示整数（无 '.'、'e'、'E'）
    bool IsIntegralLiteralText(const std::string &text);

    // 无行上下文的字面量 → 存储值。非 LITERAL 节点返回 NULL 值。
    Value LiteralToValue(const cella::CELLA_Expr &e);

    // ── 取值约束 ──────────────────────────────────────────────
    // 把输入值转换/校验为目标列类型：
    //   * NULL → NOT NULL 列：kNotNullViolation
    //   * 数值族内部互转：整数目标要求值为整（1.0 可入 INT，1.5 报 kTypeMismatch）
    //   * 字符族：超长报 kValueTooLong
    //   * 跨族（数值↔字符）：kTypeMismatch
    DbStatus CoerceValue(const Value &in, cella::CELLA_DataType target, uint16_t max_len,
                         bool not_null, Value *out);

    // ── 比较与相等（三值逻辑）────────────────────────────────
    // 任一侧为 NULL 时 *known 置 false（SQL 的 UNKNOWN），返回 0。
    int CompareValues(const Value &a, const Value &b, bool *known);

    // 用于 DISTINCT / GROUP BY：NULL == NULL（分组语义），返回是否同一值。
    bool ValueEquals(const Value &a, const Value &b);

    // ── 渲染 ─────────────────────────────────────────────────
    std::string RenderValue(const Value &v);               // 单元格文本
    std::string RenderType(const cella::CELLA_DataType t); // "VARCHAR(32)" / "INT"

} // namespace cella::db
