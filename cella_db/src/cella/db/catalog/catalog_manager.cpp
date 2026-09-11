#include "cella/db/catalog/catalog_manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cella/db/common/db_logger.h"
#include "cella/db/common/value_bridge.h"
#include "cella/storage/api/i_storage.h"
#include "cella/storage/common/status.h"
#include "cella/storage/table/table_heap.h"

namespace cella::db {
namespace {

constexpr const char* kCatalogMagic = "CELLA-CATALOG";
constexpr int kCatalogVersion = 1;

std::string ToUpper(const std::string& s) { return cella::cella_toUpper(s); }

// 把一行按空白切成 token
std::vector<std::string> SplitWs(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream is(line);
  std::string tok;
  while (is >> tok) {
    out.push_back(tok);
  }
  return out;
}

// CHAR/VARCHAR 缺省长度归一为 255（与编译器默认一致）
void NormalizeColumn(CatalogColumn* c) {
  if (c->type == cella::CELLA_DataType::CHAR || c->type == cella::CELLA_DataType::VARCHAR) {
    if (c->len <= 0) {
      c->len = 255;
    }
  } else {
    c->len = 0;
  }
}

// 目录表写操作的存储错误 → DbStatus
DbStatus CatalogStorageError(const char* what, const storage::Status& s) {
  DbCode code = DbCode::kStorageError;
  if (s.code() == storage::StatusCode::kRecordTooLarge) {
    code = DbCode::kRecordTooLarge;
  }
  return DbStatus::Error(code, std::string(what) + " [存储: " + s.ToString() + "]");
}

}  // namespace

// ── 类型名映射 ──────────────────────────────────────────────

const char* CatalogTypeName(cella::CELLA_DataType t) {
  switch (t) {
    case cella::CELLA_DataType::INT:      return "INT";
    case cella::CELLA_DataType::FLOAT:    return "FLOAT";
    case cella::CELLA_DataType::DOUBLE:   return "DOUBLE";
    case cella::CELLA_DataType::CHAR:     return "CHAR";
    case cella::CELLA_DataType::VARCHAR:  return "VARCHAR";
    case cella::CELLA_DataType::TEXT:     return "TEXT";
    case cella::CELLA_DataType::DATE:     return "DATE";
    case cella::CELLA_DataType::TIME:     return "TIME";
    case cella::CELLA_DataType::DATETIME: return "DATETIME";
  }
  return "VARCHAR";
}

bool CatalogTypeFromName(const std::string& name, cella::CELLA_DataType* out) {
  const std::string u = ToUpper(name);
  struct Pair { const char* n; cella::CELLA_DataType t; };
  static const Pair kPairs[] = {
      {"INT", cella::CELLA_DataType::INT},
      {"INTEGER", cella::CELLA_DataType::INT},
      {"FLOAT", cella::CELLA_DataType::FLOAT},
      {"DOUBLE", cella::CELLA_DataType::DOUBLE},
      {"CHAR", cella::CELLA_DataType::CHAR},
      {"VARCHAR", cella::CELLA_DataType::VARCHAR},
      {"TEXT", cella::CELLA_DataType::TEXT},
      {"DATE", cella::CELLA_DataType::DATE},
      {"TIME", cella::CELLA_DataType::TIME},
      {"DATETIME", cella::CELLA_DataType::DATETIME},
  };
  for (const auto& p : kPairs) {
    if (u == p.n) {
      if (out != nullptr) {
        *out = p.t;
      }
      return true;
    }
  }
  return false;
}

// ── CatalogTable ────────────────────────────────────────────

int CatalogTable::ColumnIndex(const std::string& column) const {
  const std::string key = ToUpper(column);
  for (size_t i = 0; i < columns.size(); ++i) {
    if (ToUpper(columns[i].name) == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const CatalogColumn* CatalogTable::FindColumn(const std::string& column) const {
  const int i = ColumnIndex(column);
  return i < 0 ? nullptr : &columns[static_cast<size_t>(i)];
}

uint16_t CatalogTable::MaxLenAt(size_t index) const {
  if (index >= columns.size()) {
    return 0;
  }
  const CatalogColumn& c = columns[index];
  if (c.type == cella::CELLA_DataType::CHAR || c.type == cella::CELLA_DataType::VARCHAR) {
    return static_cast<uint16_t>(c.len > 0 ? c.len : 255);
  }
  return 0;  // TEXT / 定长类型：不做长度约束
}

// ── CatalogManager：系统表 ─────────────────────────────────

bool CatalogManager::IsSystemTable(const std::string& name) {
  return ToUpper(name) == ToUpper(kSystemTableName);
}

storage::Schema CatalogManager::SystemTableSchema() {
  storage::Schema s;
  s.AddColumn("name", storage::ValueType::kVarchar, 64);
  s.AddColumn("table_id", storage::ValueType::kInt32, 0);
  s.AddColumn("created_at", storage::ValueType::kInt32, 0);
  s.AddColumn("columns", storage::ValueType::kVarchar, 0);
  return s;
}

DbStatus CatalogManager::EnsureSystemTable(bool* created) {
  if (created != nullptr) {
    *created = false;
  }
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "目录未附加存储引擎");
  }
  std::shared_ptr<storage::TableHeap> heap;
  const storage::Status s = storage_->open_table(kSystemTableName, &heap);
  if (s.ok()) {
    return DbStatus::Ok();
  }
  if (s.code() != storage::StatusCode::kTableNotFound) {
    return DbStatus::Error(DbCode::kStorageError,
                           "打开系统目录表失败: " + s.ToString());
  }
  const storage::Status cs = storage_->create_table(kSystemTableName, SystemTableSchema());
  if (!cs.ok()) {
    return DbStatus::Error(DbCode::kStorageError,
                           "创建系统目录表失败: " + cs.ToString());
  }
  if (created != nullptr) {
    *created = true;
  }
  DbLogInfo(logcat::kCatalog, "已创建系统目录表 " + std::string(kSystemTableName));
  return DbStatus::Ok();
}

DbStatus CatalogManager::LoadFromStorage() {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "目录未附加存储引擎");
  }
  tables_.clear();
  next_table_id_ = 1;

  std::shared_ptr<storage::TableHeap> heap;
  const storage::Status os = storage_->open_table(kSystemTableName, &heap);
  if (!os.ok()) {
    return DbStatus::Error(DbCode::kStorageError,
                           "打开系统目录表失败: " + os.ToString());
  }

  for (auto it = heap->begin(); it != heap->end(); ++it) {
    CatalogTable t;
    if (!DecodeRow(*it, &t)) {
      DbLogWarn(logcat::kCatalog, "目录表中存在无法解析的行，已跳过");
      continue;
    }
    const std::string key = ToUpper(t.name);
    if (tables_.count(key) != 0) {
      DbLogWarn(logcat::kCatalog, "目录表中存在重复表名，已跳过: " + t.name);
      continue;
    }
    tables_[key] = t;
    next_table_id_ = std::max(next_table_id_, t.table_id + 1);
  }

  // 校验：逐表 open_table 补 first_page_id；物理表已不存在的陈旧条目直接剔除
  //（而不是重建——重建会把「已删但没来得及落盘」的表又复活出来）。
  for (auto it = tables_.begin(); it != tables_.end();) {
    std::shared_ptr<storage::TableHeap> h;
    const storage::Status s = storage_->open_table(it->second.name, &h);
    if (s.ok()) {
      it->second.first_page_id = h->first_page_id();
      ++it;
    } else {
      DbLogWarn(logcat::kCatalog,
                "目录中的表 " + it->second.name + " 在数据文件里缺失，已从目录剔除");
      it = tables_.erase(it);
    }
  }

  AddSystemTableEntry();
  DbLogInfo(logcat::kCatalog, "目录已加载: " + std::to_string(tables_.size()) + " 张表");
  return DbStatus::Ok();
}

void CatalogManager::AddSystemTableEntry() {
  CatalogTable sys;
  sys.table_id = 0;
  sys.name = kSystemTableName;
  sys.created_at = 0;

  CatalogColumn c;
  c.name = "name"; c.type = cella::CELLA_DataType::VARCHAR; c.len = 64; c.not_null = true;
  sys.columns.push_back(c);
  c = CatalogColumn{}; c.name = "table_id"; c.type = cella::CELLA_DataType::INT; c.not_null = true;
  sys.columns.push_back(c);
  c = CatalogColumn{}; c.name = "created_at"; c.type = cella::CELLA_DataType::INT; c.not_null = true;
  sys.columns.push_back(c);
  c = CatalogColumn{}; c.name = "columns"; c.type = cella::CELLA_DataType::TEXT;
  sys.columns.push_back(c);

  std::shared_ptr<storage::TableHeap> h;
  if (storage_ != nullptr && storage_->open_table(kSystemTableName, &h).ok() && h) {
    sys.first_page_id = h->first_page_id();
  }
  tables_[ToUpper(kSystemTableName)] = std::move(sys);
}

bool CatalogManager::DecodeRow(const storage::Record& row, CatalogTable* out) const {
  if (row.value_count() < 4) {
    return false;
  }
  const storage::Value& name = row.value(0);
  const storage::Value& tid = row.value(1);
  const storage::Value& cat = row.value(2);
  const storage::Value& cols = row.value(3);
  if (name.type != storage::ValueType::kVarchar ||
      tid.type != storage::ValueType::kInt32 ||
      cat.type != storage::ValueType::kInt32 ||
      cols.type != storage::ValueType::kVarchar) {
    return false;
  }
  out->name = name.str_val;
  out->table_id = static_cast<uint32_t>(tid.int32_val);
  out->created_at = static_cast<int64_t>(cat.int32_val);
  out->first_page_id = storage::kInvalidPageId;
  return DecodeColumns(cols.str_val, &out->columns);
}

storage::Rid CatalogManager::FindCatalogRow(const std::string& name) const {
  storage::Rid none;
  if (storage_ == nullptr) {
    return none;
  }
  std::shared_ptr<storage::TableHeap> heap;
  if (!storage_->open_table(kSystemTableName, &heap).ok()) {
    return none;
  }
  const std::string key = ToUpper(name);
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    const storage::Record& rec = *it;
    if (rec.value_count() > 0 && rec.value(0).type == storage::ValueType::kVarchar &&
        ToUpper(rec.value(0).str_val) == key) {
      return it.rid();
    }
  }
  return none;
}

DbStatus CatalogManager::WriteTableRow(const CatalogTable& table) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "目录未附加存储引擎");
  }
  storage::Record rec;
  rec.AddValue(storage::Value::Varchar(table.name));
  rec.AddValue(storage::Value::Int(static_cast<int32_t>(table.table_id)));
  rec.AddValue(storage::Value::Int(static_cast<int32_t>(table.created_at)));
  rec.AddValue(storage::Value::Varchar(EncodeColumns(table.columns)));
  storage::Rid rid;
  const storage::Status s = storage_->insert_record(kSystemTableName, rec, &rid);
  if (!s.ok()) {
    return CatalogStorageError("写系统目录表失败", s);
  }
  return DbStatus::Ok();
}

DbStatus CatalogManager::DeleteTableRow(const std::string& name) {
  const storage::Rid rid = FindCatalogRow(name);
  if (!rid.IsValid()) {
    return DbStatus::Error(DbCode::kCatalogError, "目录表中未找到表行: " + name);
  }
  const storage::Status s = storage_->delete_record(kSystemTableName, rid);
  if (!s.ok()) {
    return CatalogStorageError("删系统目录表行失败", s);
  }
  return DbStatus::Ok();
}

DbStatus CatalogManager::EnsurePhysicalTable(const CatalogTable& table) {
  if (storage_ == nullptr) {
    return DbStatus::Error(DbCode::kCatalogError, "目录未附加存储引擎");
  }
  std::shared_ptr<storage::TableHeap> heap;
  const storage::Status os = storage_->open_table(table.name, &heap);
  if (os.ok()) {
    return DbStatus::Ok();  // 物理表已存在，无需重建
  }
  if (os.code() != storage::StatusCode::kTableNotFound) {
    return DbStatus::Error(DbCode::kStorageError,
                           "打开表 " + table.name + " 失败: " + os.ToString());
  }
  // 按目录里的列定义重建物理表（结构保住；数据本就不可恢复）
  storage::Schema schema;
  for (size_t i = 0; i < table.columns.size(); ++i) {
    schema.AddColumn(table.columns[i].name, ToStorageType(table.columns[i].type),
                     table.MaxLenAt(i));
  }
  const storage::Status cs = storage_->create_table(table.name, schema);
  if (!cs.ok()) {
    return DbStatus::Error(DbCode::kStorageError,
                           "迁移建表 " + table.name + " 失败: " + cs.ToString());
  }
  DbLogInfo(logcat::kCatalog, "迁移：已按目录重建物理表 " + table.name +
                                  "（" + std::to_string(table.columns.size()) + " 列）");
  return DbStatus::Ok();
}

std::string CatalogManager::EncodeColumns(const std::vector<CatalogColumn>& cols) {
  std::ostringstream os;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    const CatalogColumn& c = cols[i];
    os << c.name << " " << CatalogTypeName(c.type) << " " << c.len << " " << (c.not_null ? 1 : 0);
  }
  return os.str();
}

bool CatalogManager::DecodeColumns(const std::string& text, std::vector<CatalogColumn>* cols) {
  cols->clear();
  if (text.empty()) {
    return true;  // 零列（防御，正常建表不会出现）
  }
  std::istringstream is(text);
  std::string part;
  while (std::getline(is, part, ',')) {
    const std::vector<std::string> t = SplitWs(part);
    if (t.size() != 4) {
      return false;
    }
    CatalogColumn c;
    c.name = t[0];
    if (!CatalogTypeFromName(t[1], &c.type)) {
      return false;
    }
    c.len = std::atoi(t[2].c_str());
    c.not_null = (t[3] == "1");
    cols->push_back(std::move(c));
  }
  return true;
}

// ── 旧文本格式迁移 ──────────────────────────────────────────

DbStatus CatalogManager::LoadLegacyText(const std::string& data_dir) {
  data_dir_ = data_dir;
  tables_.clear();
  next_table_id_ = 1;

  const std::string path = data_dir_ + "/" + LegacyFileName();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return DbStatus::Ok();  // 文件不存在：视为无需迁移
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();

  std::istringstream lines(text);
  std::string line;
  bool header_ok = false;
  CatalogTable current;
  bool have_current = false;
  int line_no = 0;

  auto flush_current = [&]() -> DbStatus {
    if (!have_current) {
      return DbStatus::Ok();
    }
    for (auto& c : current.columns) {
      NormalizeColumn(&c);
    }
    const std::string key = ToUpper(current.name);
    if (tables_.count(key) != 0) {
      return DbStatus::Error(DbCode::kCatalogError, "目录中存在重复表名: " + current.name);
    }
    tables_[key] = std::move(current);
    current = CatalogTable{};
    have_current = false;
    return DbStatus::Ok();
  };

  while (std::getline(lines, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::vector<std::string> t = SplitWs(line);
    if (t.empty()) {
      continue;
    }
    if (!header_ok) {
      if (t[0] != kCatalogMagic || t.size() < 2) {
        return DbStatus::Error(DbCode::kCatalogError,
                               "目录文件头非法（期望 " + std::string(kCatalogMagic) + "）: " + path);
      }
      const int ver = std::atoi(t[1].c_str());
      if (ver != kCatalogVersion) {
        return DbStatus::Error(DbCode::kCatalogError,
                               "目录版本不支持: " + std::to_string(ver) + "（本实现支持 " +
                                   std::to_string(kCatalogVersion) + "）");
      }
      header_ok = true;
      continue;
    }
    if (t[0] == "table") {
      const DbStatus fs = flush_current();
      if (!fs.ok()) {
        return fs;
      }
      if (t.size() < 6) {
        return DbStatus::Error(DbCode::kCatalogError,
                               "目录第 " + std::to_string(line_no) + " 行 table 字段不足");
      }
      current.table_id = static_cast<uint32_t>(std::strtoul(t[1].c_str(), nullptr, 10));
      current.name = t[2];
      current.first_page_id =
          static_cast<storage::page_id_t>(std::strtoul(t[4].c_str(), nullptr, 10));
      current.created_at = static_cast<int64_t>(std::strtoll(t[5].c_str(), nullptr, 10));
      current.columns.clear();
      have_current = true;
      next_table_id_ = std::max(next_table_id_, current.table_id + 1);
    } else if (t[0] == "col") {
      if (!have_current) {
        return DbStatus::Error(DbCode::kCatalogError,
                               "目录第 " + std::to_string(line_no) + " 行 col 出现在 table 之前");
      }
      if (t.size() < 5) {
        return DbStatus::Error(DbCode::kCatalogError,
                               "目录第 " + std::to_string(line_no) + " 行 col 字段不足");
      }
      CatalogColumn c;
      c.name = t[1];
      if (!CatalogTypeFromName(t[2], &c.type)) {
        return DbStatus::Error(DbCode::kCatalogError, "未知列类型: " + t[2]);
      }
      c.len = std::atoi(t[3].c_str());
      c.not_null = (t[4] == "1");
      current.columns.push_back(std::move(c));
    } else {
      return DbStatus::Error(DbCode::kCatalogError,
                             "目录第 " + std::to_string(line_no) + " 行未知记录类型: " + t[0]);
    }
  }
  const DbStatus fs = flush_current();
  if (!fs.ok()) {
    return fs;
  }
  if (!header_ok) {
    return DbStatus::Error(DbCode::kCatalogError, "目录文件为空或缺少头: " + path);
  }
  DbLogInfo(logcat::kCatalog, "旧目录已解析: " + std::to_string(tables_.size()) + " 张表");
  return DbStatus::Ok();
}

// ── 内存目录：查询 / 登记 / 删除 ────────────────────────────

DbStatus CatalogManager::RegisterTable(const CatalogTable& table) {
  if (table.name.empty()) {
    return DbStatus::Error(DbCode::kCatalogError, "表名不能为空");
  }
  const std::string key = ToUpper(table.name);
  if (tables_.count(key) != 0) {
    return DbStatus::Error(DbCode::kTableExists, "表已存在: " + table.name);
  }
  CatalogTable entry = table;
  for (auto& c : entry.columns) {
    NormalizeColumn(&c);
  }
  if (entry.table_id == 0) {
    entry.table_id = AllocateTableId();
  } else {
    next_table_id_ = std::max(next_table_id_, entry.table_id + 1);
  }
  if (entry.created_at == 0) {
    entry.created_at = static_cast<int64_t>(std::time(nullptr));
  }
  tables_[key] = std::move(entry);
  return DbStatus::Ok();
}

DbStatus CatalogManager::RemoveTable(const std::string& name) {
  const std::string key = ToUpper(name);
  const auto it = tables_.find(key);
  if (it == tables_.end()) {
    return DbStatus::Error(DbCode::kTableNotFound, "表不存在: " + name);
  }
  tables_.erase(it);
  return DbStatus::Ok();
}

const CatalogTable* CatalogManager::FindTable(const std::string& name) const {
  const auto it = tables_.find(ToUpper(name));
  return it == tables_.end() ? nullptr : &it->second;
}

std::string CatalogManager::CanonicalName(const std::string& name) const {
  const CatalogTable* t = FindTable(name);
  return t == nullptr ? name : t->name;
}

std::vector<const CatalogTable*> CatalogManager::ListTables() const {
  std::vector<const CatalogTable*> out;
  out.reserve(tables_.size());
  for (const auto& kv : tables_) {
    out.push_back(&kv.second);
  }
  std::sort(out.begin(), out.end(), [](const CatalogTable* a, const CatalogTable* b) {
    return a->table_id < b->table_id;
  });
  return out;
}

cella::CELLA_Catalog CatalogManager::ToCompilerCatalog() const {
  cella::CELLA_Catalog catalog;
  for (const auto* t : ListTables()) {
    cella::CELLA_Table ct;
    ct.name = t->name;
    for (const auto& c : t->columns) {
      cella::CELLA_Column cc;
      cc.name = c.name;
      cc.type = c.type;
      cc.len = c.len;
      cc.notNull = c.not_null;
      ct.columns.push_back(std::move(cc));
    }
    catalog.addTable(std::move(ct));
  }
  return catalog;
}

std::string CatalogManager::Describe() const {
  std::ostringstream os;
  const auto tables = ListTables();
  os << "表数量: " << tables.size() << "\n";
  for (const auto* t : tables) {
    os << "  #" << t->table_id << " " << t->name << " ("
       << t->columns.size() << " 列, 首数据页 " << t->first_page_id << ")\n";
  }
  return os.str();
}

std::string CatalogManager::DescribeTable(const std::string& name) const {
  const CatalogTable* t = FindTable(name);
  if (t == nullptr) {
    return "表不存在: " + name;
  }
  std::ostringstream os;
  os << t->name << " (表号 #" << t->table_id << ", 首数据页 " << t->first_page_id << ")\n";
  for (size_t i = 0; i < t->columns.size(); ++i) {
    const CatalogColumn& c = t->columns[i];
    os << "  " << (i + 1) << ". " << c.name << " " << CatalogTypeName(c.type);
    if (c.type == cella::CELLA_DataType::CHAR || c.type == cella::CELLA_DataType::VARCHAR) {
      os << "(" << c.len << ")";
    }
    if (c.not_null) {
      os << " NOT NULL";
    }
    os << "\n";
  }
  return os.str();
}

}  // namespace cella::db
