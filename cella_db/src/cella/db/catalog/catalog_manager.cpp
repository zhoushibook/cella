#include "cella/db/catalog/catalog_manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cella/db/common/db_logger.h"

namespace cella::db {
namespace {

constexpr const char* kCatalogMagic = "CELLA-CATALOG";
constexpr int kCatalogVersion = 1;
constexpr const char* kCatalogFileName = "catalog.meta";

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

// ── CatalogManager ─────────────────────────────────────────

std::string CatalogManager::catalog_path() const {
  if (data_dir_.empty()) {
    return kCatalogFileName;
  }
  return data_dir_ + "/" + kCatalogFileName;
}

DbStatus CatalogManager::Load(const std::string& data_dir) {
  data_dir_ = data_dir;
  tables_.clear();
  next_table_id_ = 1;

  const std::string path = catalog_path();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    DbLogInfo(logcat::kCatalog, "未找到目录文件 " + path + "，按空库启动");
    return DbStatus::Ok();  // 全新库
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();

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
      current.first_page_id = static_cast<storage::page_id_t>(std::strtoul(t[4].c_str(), nullptr, 10));
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
  DbLogInfo(logcat::kCatalog, "目录已加载: " + std::to_string(tables_.size()) + " 张表");
  return DbStatus::Ok();
}

DbStatus CatalogManager::Save() const {
  if (data_dir_.empty()) {
    return DbStatus::Error(DbCode::kCatalogError, "未设置数据目录");
  }
  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  std::ostringstream os;
  os << kCatalogMagic << " " << kCatalogVersion << "\n";
  for (const auto& kv : tables_) {
    const CatalogTable& t = kv.second;
    os << "table " << t.table_id << " " << t.name << " " << t.columns.size() << " "
       << t.first_page_id << " " << t.created_at << "\n";
    for (const auto& c : t.columns) {
      os << "col " << c.name << " " << CatalogTypeName(c.type) << " " << c.len << " "
         << (c.not_null ? 1 : 0) << "\n";
    }
  }

  const std::string path = catalog_path();
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return DbStatus::Error(DbCode::kCatalogError, "无法写入临时目录文件: " + tmp);
    }
    const std::string text = os.str();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
      return DbStatus::Error(DbCode::kCatalogError, "写目录文件失败: " + tmp);
    }
  }
  std::filesystem::rename(tmp, path, ec);  // MSVC 实现为替换式改名
  if (ec) {
    return DbStatus::Error(DbCode::kCatalogError,
                           "目录文件原子替换失败: " + ec.message());
  }
  return DbStatus::Ok();
}

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
