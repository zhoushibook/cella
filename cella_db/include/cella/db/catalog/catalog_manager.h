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
  bool primary_key = false;               // 主键列（隐含 not_null）
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
  // 主键列下标序列（按列声明序）；空 = 无主键。复合主键 = 多个下标。
  std::vector<int> PrimaryKeyColumns() const;
  // 单列主键下标；无主键**或复合主键**返回 -1（主键索引等单列路径据此跳过复合表）
  int PrimaryKeyColumnIndex() const;
};

// ── 一个二级索引的元数据（P1.2；复合索引见 columns）─────────
// 存放在独立的系统表 cella_index 中（不塞进 cella_catalog，避免改动既有行格式
// 与 golden 输出）。一行一个索引。
// 行格式（6 列，**不动**）：name | table | column | unique | root_page_id | created_at
// 复合索引的列清单存进 column 列：逗号拼接 "a,b"（标识符不含逗号，解析安全）。
struct CatalogIndex {
  std::string name;                  // 索引名（原始拼写）
  std::string table;                 // 所属表名（原始拼写）
  std::string column;                // 列清单的逗号拼接形式（行格式兼容字段）
  std::vector<std::string> columns;  // 索引列清单（按声明序；权威字段）
  bool unique = false;               // 唯一索引
  uint32_t root_page_id = 0;         // B+ 树根页号
  int64_t created_at = 0;            // Unix 秒

  // 归属表存在且列清单有效时有效
  bool valid() const { return !name.empty() && !table.empty() && !columns.empty(); }
  // 列清单文本（写库/展示统一走这里，保证与 column 字段一致）
  std::string JoinedColumns() const;
};

// 索引列清单 ⇄ 逗号拼接文本（cella_index 行格式；标识符不含逗号）
std::string JoinIndexColumns(const std::vector<std::string>& cols);
std::vector<std::string> SplitIndexColumns(const std::string& joined);

// ── 目录管理器 ──────────────────────────────────────────────
class CatalogManager {
 public:
  CatalogManager() = default;
  CatalogManager(const CatalogManager&) = delete;
  CatalogManager& operator=(const CatalogManager&) = delete;

  // ── 系统表相关 ────────────────────────────────────────────
  static constexpr const char* kSystemTableName = "cella_catalog";
  // 二级索引元数据表（独立系统表；P1.2）
  static constexpr const char* kIndexTableName = "cella_index";
  static bool IsSystemTable(const std::string& name);
  // 是否为只读系统表（cella_catalog / cella_index 都只读）
  static bool IsProtectedSystemTable(const std::string& name);

  // 附加存储引擎（读写系统表用）。写接口须在持有 storage_mutex_ 的临界区内调用。
  void AttachStorage(storage::IStorage* storage) { storage_ = storage; }

  // 确保系统目录表存在（不存在则创建）。返回是否「本次新建」。
  DbStatus EnsureSystemTable(bool* created);

  // 确保索引系统表存在（不存在则创建）。返回是否「本次新建」。
  DbStatus EnsureIndexTable(bool* created);

  // 从系统目录表全表扫描重建内存目录；逐表 open_table 校验并补 first_page_id。
  // 校验失败的陈旧条目（表名在目录里、物理表却没了）会被剔除并告警，而不是重建。
  DbStatus LoadFromStorage();

  // 索引系统表：全量重建内存索引视图（在 LoadFromStorage 之后调用）。
  DbStatus LoadIndexesFromStorage();

  // ── 二级索引元数据 ────────────────────────────────────────
  // 写/删一条索引元数据（供执行器 DDL 调用；须在 storage_mutex_ 临界区内）。
  DbStatus WriteIndexRow(const CatalogIndex& index);
  DbStatus DeleteIndexRows(const std::string& index_name);
  // 树根分裂后回写根页号：旧元数据行打墓碑 + 追加带新根页号的行，并就地更新
  // 内存视图。根页号没变时零开销直接返回（DML 逐行调用不会产生多余 I/O）。
  // 须在 storage_mutex_ 临界区内调用。
  DbStatus UpdateIndexRoot(const std::string& index_name, uint32_t root_page_id);
  // 删除一张表的全部索引元数据（DROP TABLE 级联；须在临界区内）。
  DbStatus DeleteIndexesOfTable(const std::string& table_name);

  // 内存索引视图查询
  const CatalogIndex* FindIndex(const std::string& index_name) const;
  std::vector<const CatalogIndex*> IndexesOfTable(const std::string& table_name) const;
  std::vector<const CatalogIndex*> ListIndexes() const;
  size_t index_count() const { return indexes_.size(); }

  // 写/删一张用户表的目录行（供执行器 DDL 调用；须在 storage_mutex_ 临界区内）。
  DbStatus WriteTableRow(const CatalogTable& table);
  DbStatus DeleteTableRow(const std::string& name);

  // ── ALTER TABLE 支持（P5）───────────────────────────────────
  // 整表元数据替换（支持改名）：把 old_name 的目录行与内存条目换成 table。
  // 为什么需要它：ADD/DROP COLUMN / RENAME / ADD|DROP PRIMARY KEY 都只改「列定义」，
  // 而 WriteTableRow 只会**追加**一行（表名相同就会出现两行目录，重启后解码出重复表）。
  // 因此这里显式「先删旧行（新旧两个名字都清一遍）再写新行」。
  // 表号（table_id）保持不变 —— 它是元数据的稳定身份，不应随 ALTER 变动。
  // 须在 storage_mutex_ 临界区内调用。
  DbStatus ReplaceTableMeta(const std::string& old_name, const CatalogTable& table);

  // 整条索引元数据替换（索引改名 / 改列清单 / 换 root 页之后调用）。
  // 实现 = 先删同名旧行（含内存条目）再写新行，因此天然幂等。
  // 须在 storage_mutex_ 临界区内调用。
  DbStatus ReplaceIndexMeta(const CatalogIndex& index);

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
  static storage::Schema IndexTableSchema();
  storage::Rid FindCatalogRow(const std::string& name) const;
  storage::Rid FindIndexRow(const std::string& index_name) const;
  // 扫描到的每行 → CatalogTable；失败返回 false
  bool DecodeRow(const storage::Record& row, CatalogTable* out) const;
  // 索引系统表每行 → CatalogIndex；失败返回 false
  bool DecodeIndexRow(const storage::Record& row, CatalogIndex* out) const;
  // 把系统表自身作为一条合成条目放进内存目录（可查、可 FindTable）
  void AddSystemTableEntry();
  // 把索引系统表作为一条合成条目放进内存目录（可查）
  void AddIndexTableEntry();

  std::string data_dir_;
  uint32_t next_table_id_ = 1;
  std::map<std::string, CatalogTable> tables_;  // 键 = 大写表名
  std::map<std::string, CatalogIndex> indexes_; // 键 = 大写索引名
  storage::IStorage* storage_ = nullptr;
};

// SQL 类型 ↔ 文本（columns 编码里的 type 字段）
const char* CatalogTypeName(cella::CELLA_DataType t);
bool CatalogTypeFromName(const std::string& name, cella::CELLA_DataType* out);

}  // namespace cella::db
