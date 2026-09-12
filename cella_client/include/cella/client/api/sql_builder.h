// sql_builder.h —— 客户端语义 → cella 方言 SQL。
//
// 这里集中了所有「替用户写 SQL」的逻辑，是数据浏览/编辑 correctness 的核心：
//   * 方言事实（见 PLAN_web_client.md §11 实测）：
//       - WHERE 写作 limit，ORDER BY 写作 ordered，行数写作 among；
//       - 原生分页 `page 页码, 每页行数`，页码从 1 起；
//       - 保留字无法作列名/表名 → 目录里的名字原样拼接**不需要**转义；
//       - `*` 是语句级独占标志 → 取 rowid 必须显式列清单；
//       - NULL 条件必须写 `col is null`，`= NULL` 永不成立。
//   * 定位键统一为**元组** RowKey（单列主键 = 长度 1），为表级复合主键预留。
#pragma once

#include <string>
#include <vector>

#include "cella/client/api/json.h"
#include "cella/cella_catalog.h"
#include "cella/db/catalog/catalog_manager.h"
#include "cella/db/common/db_status.h"
#include "cella/storage/common/value.h"

namespace cella::client {

// ── 行定位键（元组形态；kind 决定安全性，见 PLAN §6.1 三级矩阵）──
struct RowKey {
  enum class Kind { kPrimary, kRowid, kFullRow };
  Kind kind = Kind::kFullRow;
  std::vector<std::string> columns;
  std::vector<cella::storage::Value> values;
};

// ── 基础 ────────────────────────────────────────────────────
// 标识符合法性兜底（目录保证合法，这里防目录异常态/注入式构造）
bool ValidIdentifier(const std::string& s);

// Value → SQL 字面量：NULL/数字/字符串（'' 转义）；BOOL → true/false
std::string SqlLiteral(const cella::storage::Value& v);

// WHERE 片段（不含 "limit" 关键字）：NULL 列生成 "col is null"，其余 "col = 字面量"
std::string KeyCondition(const RowKey& key);

// 默认排序列（D9）：有主键 → 主键列；否则 rowid
std::string DefaultSortColumn(const cella::db::CatalogTable& table);

// ── 语句生成（全部以 ';' 结尾）─────────────────────────────
// get rowid, c1, c2 in T ordered X asc page N, M
std::string SelectPageSql(const std::string& table, const std::vector<std::string>& columns,
                          const std::string& order_col, bool desc, int page, int page_size);

// get c1, c2 in T limit <key>        （乐观校验的重取）
std::string SelectByKeySql(const std::string& table, const std::vector<std::string>& columns,
                           const RowKey& key);

// get c1 in T                        （懒计数：取一列全量后计数）
std::string CountSql(const std::string& table, const std::string& first_col);

// INSERT INTO T(c1, c2) VALUES (...);（显式全列清单，空值显式 NULL）
std::string InsertSql(const std::string& table, const std::vector<std::string>& cols,
                      const std::vector<cella::storage::Value>& vals);

// UPDATE T SET c = v, ... limit <key>;
std::string UpdateSql(const std::string& table, const std::vector<std::string>& set_cols,
                      const std::vector<cella::storage::Value>& set_vals, const RowKey& key);

// DELETE in T limit <key>;
std::string DeleteSql(const std::string& table, const RowKey& key);

// ── JSON → 存储值（按列声明类型收敛）────────────────────────
// declared_type 形如 "INT" / "VARCHAR" / "DOUBLE"（来自 CatalogTypeName）。
// JSON null → NULL；INT 要求整数；FLOAT/DOUBLE 要求数值；字符族要求字符串。
bool CellFromJson(const JsonValue& v, const std::string& declared_type,
                  cella::storage::Value* out, std::string* err);

// 由目录列清单推导主键元组（无主键返回 false；key 可为 nullptr 仅探测）
bool PrimaryKeyOf(const cella::db::CatalogTable& table, RowKey* key);

// 由 rowid 构造定位键（kRowid；O(1) 直达路径依赖 `limit rowid = X`）
RowKey RowidKey(std::int64_t rowid);

}  // namespace cella::client
