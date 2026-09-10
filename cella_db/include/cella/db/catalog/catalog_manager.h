// catalog_manager.h —— 表元数据（catalog）管理：持久化 + 查询 + 与存储层同步。
//
// 为什么整合层还要自己一份目录？
//   存储层的 FileStorage 内部确实持久化了一份「最小目录」（表名 → schema + 首数据页），
//   但它只服务于「页/表堆」自身，且 IStorage 没有「列出所有表」的接口。
//   数据库层需要的是「富语义目录」：列长度、NOT NULL、建表时间、稳定表号，
//   并且要能枚举、能落盘、能被 DDL 事务化处理。因此本层持有权威目录，
//   启动时与存储层目录互相校验：
//     * 本层有、存储层无  → 说明数据文件被替换，报 DB-509
//     * 本层无、存储层有  → 由本层补写元数据（以存储层 schema 为准，容错恢复）
//
// 持久化格式（<data_dir>/catalog.meta，UTF-8 文本，行式）：
//     CELLA-CATALOG 1
//     table <table_id> <name> <col_count> <first_page_id> <created_at>
//     col <name> <type> <max_len> <not_null>
//     ...
//   名称为 SQL 标识符，不含空白，故用空白分隔即可；行尾 CRLF/LF 均可。
//   写入采用「临时文件 + 原子改名」，避免写一半留下坏目录。
#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cella/cella_catalog.h"
#include "cella/db/common/db_status.h"
#include "cella/storage/common/types.h"

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
  uint32_t table_id = 0;                  // 稳定表号（自增，永不复用）
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

  // 读取 <data_dir>/catalog.meta；文件不存在视为空库（返回 OK）
  DbStatus Load(const std::string& data_dir);
  // 原子写回
  DbStatus Save() const;

  // 登记一张表（不落盘；由调用方决定 Save 时机）。表已存在返回 kTableExists。
  DbStatus RegisterTable(const CatalogTable& table);
  // 移除一张表（不落盘）
  DbStatus RemoveTable(const std::string& name);

  const CatalogTable* FindTable(const std::string& name) const;
  // 按原始拼写/大写键都接受；返回实际存储名
  std::string CanonicalName(const std::string& name) const;

  // 全部表（按表号升序，输出稳定）
  std::vector<const CatalogTable*> ListTables() const;
  size_t table_count() const { return tables_.size(); }

  uint32_t AllocateTableId() { return next_table_id_++; }
  uint32_t next_table_id() const { return next_table_id_; }

  const std::string& data_dir() const { return data_dir_; }
  std::string catalog_path() const;

  // 供编译器语义分析使用的只读视图（表名/列/类型/长度/NOT NULL）
  cella::CELLA_Catalog ToCompilerCatalog() const;

  // 诊断：多行文本（表名 列定义 首数据页）
  std::string Describe() const;
  // 单表描述
  std::string DescribeTable(const std::string& name) const;

 private:
  std::string data_dir_;
  uint32_t next_table_id_ = 1;
  std::map<std::string, CatalogTable> tables_;  // 键 = 大写表名
};

// SQL 类型 ↔ 文本（catalog.meta 的 type 字段）
const char* CatalogTypeName(cella::CELLA_DataType t);
bool CatalogTypeFromName(const std::string& name, cella::CELLA_DataType* out);

}  // namespace cella::db
