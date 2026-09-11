// catalog_manager.h —— 表元数据（系统目录）管理。
//
// 持久化方式：系统目录不再是一个独立文本文件，而是**一张存于页式存储里的特殊表**
// （cella_catalog）。它是数据库自包含单文件的关键：元数据与数据走同一条持久化路径
// （页式存储 + 缓冲池刷盘），「目录与数据文件不一致」这一类问题从机制上消失。
//
// 层次分工（对照存储层内部目录页）：
//   * 存储层内部目录页   —— 纯物理定位：表名 → 首数据页 + 行布局所需最小 schema，
//                           由 FileStorage 自己读写，不承载任何数据库语义；
//   * 本系统表 cella_catalog —— 逻辑权威：表号 / 建表时间 / 列定义（类型/长度/NOT NULL），
//                           可被 SQL 查询（get * in cella_catalog）。
//
// 系统表行格式（一行一张用户表，系统表自身不在其中登记）：
//   name VARCHAR(64) | table_id INT | created_at INT | columns TEXT
//   columns 列为「列定义编码」：`名 类型 长度 NOTNULL` 用 ',' 分隔多列，
//   如 "id INT 0 1, name VARCHAR 32 0"。
//
// 写保护：cella_catalog 对 SQL 只读。CREATE 由语义阶段拦（系统表在编译器目录里，
// 自然报「表已存在」）；DROP/INSERT/UPDATE/DELETE 由执行器拦（DB-512）。
#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cella/cella_catalog.h"
#include "cella/db/common/db_status.h"
#include "cella/storage/common/record.h"
#include "cella/storage/common/schema.h"
#include "cella/storage/common/types.h"

namespace cella::storage {
class IStorage;
}  // namespace cella::storage

namespace cella::db {

// ── 一列的元数据 ────────────────────────────────────────────
struct CatalogColumn {
  std::string name;                       // 原始拼写
  cella::CELLA_DataType type = cella::CELLA_DataType::INT;
  int len = 0;                            // CHAR/VARCHAR 声明长度；其余为 0
  bool not_null = false;
};

// ── 一张表的元数据 ──────────────────────────────────────────
struct CatalogTable {
  uint32_t table_id = 0;                  // 稳定表号（自增，永不复用）；系统表恒为 0
  std::string name;                       // 原始拼写；键为大写
  std::vector<CatalogColumn> columns;
  storage::page_id_t first_page_id = storage::kInvalidPageId;
  int64_t created_at = 0;                 // Unix 秒

  // 列查找（大小写不敏感）；未命中返回 -1
  int ColumnIndex(const std::string& column) const;
  const CatalogColumn* FindColumn(const std::string& column) const;
  // 存储层最大长度（CHAR/VARCHAR 截断用；TEXT/定长返回 0）
  uint16_t MaxLenAt(size_t index) const;
};

// ── 目录管理器 ──────────────────────────────────────────────
class CatalogManager {
 public:
  CatalogManager() = default;
  CatalogManager(const CatalogManager&) = delete;
  CatalogManager& operator=(const CatalogManager&) = delete;

  // ── 系统表相关 ────────────────────────────────────────────
  static constexpr const char* kSystemTableName = "cella_catalog";
  static bool IsSystemTable(const std::string& name);

  // 附加存储引擎（读写系统表用）。写接口须在持有 storage_mutex_ 的临界区内调用。
  void AttachStorage(storage::IStorage* storage) { storage_ = storage; }

  // 确保系统目录表存在（不存在则创建）。返回是否「本次新建」。
  DbStatus EnsureSystemTable(bool* created);

  // 从系统目录表全表扫描重建内存目录；逐表 open_table 校验并补 first_page_id。
  // 校验失败的陈旧条目（表名在目录里、物理表却没了）会被剔除并告警，而不是重建。
  DbStatus LoadFromStorage();

  // 写/删一张用户表的目录行（供执行器 DDL 调用；须在 storage_mutex_ 临界区内）。
  DbStatus WriteTableRow(const CatalogTable& table);
  DbStatus DeleteTableRow(const std::string& name);

  // 按目录条目确保物理表存在（迁移用）：open_table 成功则跳过；
  // 返回 kTableNotFound 则按列定义在存储层创建空表。须在 storage_mutex_ 临界区内调用。
  DbStatus EnsurePhysicalTable(const CatalogTable& table);

  // ── 旧格式迁移 ────────────────────────────────────────────
  // 解析旧文本 catalog.meta（仅当文件存在）；结果填入内存，供调用方写回系统表。
  DbStatus LoadLegacyText(const std::string& data_dir);
  static const char* LegacyFileName() { return "catalog.meta"; }
  static const char* MigratedFileName() { return "catalog.meta.migrated"; }

  // ── 内存目录（查询 / 登记 / 删除）────────────────────────
  // 登记一张表（只改内存；持久化由 WriteTableRow 完成）。表已存在返回 kTableExists。
  DbStatus RegisterTable(const CatalogTable& table);
  DbStatus RemoveTable(const std::string& name);

  const CatalogTable* FindTable(const std::string& name) const;
  std::string CanonicalName(const std::string& name) const;

  std::vector<const CatalogTable*> ListTables() const;
  size_t table_count() const { return tables_.size(); }

  uint32_t AllocateTableId() { return next_table_id_++; }
  uint32_t next_table_id() const { return next_table_id_; }

  const std::string& data_dir() const { return data_dir_; }

  // 供编译器语义分析使用的只读视图（含系统表，因此 get * in cella_catalog 可用）
  cella::CELLA_Catalog ToCompilerCatalog() const;

  // 诊断：多行文本（表名 列定义 首数据页）
  std::string Describe() const;
  std::string DescribeTable(const std::string& name) const;

 private:
  // 列定义编码 ⇄ 解码（columns 文本）
  static std::string EncodeColumns(const std::vector<CatalogColumn>& cols);
  static bool DecodeColumns(const std::string& text, std::vector<CatalogColumn>* cols);
  static storage::Schema SystemTableSchema();
  storage::Rid FindCatalogRow(const std::string& name) const;
  // 扫描到的每行 → CatalogTable；失败返回 false
  bool DecodeRow(const storage::Record& row, CatalogTable* out) const;
  // 把系统表自身作为一条合成条目放进内存目录（可查、可 FindTable）
  void AddSystemTableEntry();

  std::string data_dir_;
  uint32_t next_table_id_ = 1;
  std::map<std::string, CatalogTable> tables_;  // 键 = 大写表名
  storage::IStorage* storage_ = nullptr;
};

// SQL 类型 ↔ 文本（columns 编码里的 type 字段）
const char* CatalogTypeName(cella::CELLA_DataType t);
bool CatalogTypeFromName(const std::string& name, cella::CELLA_DataType* out);

}  // namespace cella::db
