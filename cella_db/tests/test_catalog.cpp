// test_catalog.cpp —— 系统目录测试：内存目录（登记/查询/删除/编译器视图）、
//                     系统表可查询、写保护、重启持久化、单文件自包含、旧格式迁移。
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "cella/db/catalog/catalog_manager.h"
#include "cella/storage/table/table_heap.h"
#include "cella/db/engine/db_engine.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;
using testutil::Engine;
using testutil::RowsText;

namespace {

CatalogTable MakeTable(const std::string& name) {
  CatalogTable t;
  t.name = name;
  CatalogColumn c1;
  c1.name = "id";
  c1.type = cella::CELLA_DataType::INT;
  c1.not_null = true;
  CatalogColumn c2;
  c2.name = "name";
  c2.type = cella::CELLA_DataType::VARCHAR;
  c2.len = 32;
  t.columns.push_back(c1);
  t.columns.push_back(c2);
  return t;
}

// 在「已存在的数据目录」上开一个引擎（不清理）
std::unique_ptr<DbEngine> OpenOn(const std::string& dir) {
  auto e = std::make_unique<DbEngine>();
  EngineConfig c;
  c.data_dir = dir;
  c.enable_log = false;
  c.enable_journal = true;
  if (!e->Open(c).ok()) {
    return nullptr;
  }
  return e;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::string();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

MT_TEST(目录_系统表识别) {
  MT_CHECK(CatalogManager::IsSystemTable("cella_catalog"));
  MT_CHECK(CatalogManager::IsSystemTable("CELLA_CATALOG"));
  MT_CHECK(!CatalogManager::IsSystemTable("student"));
  MT_CHECK(!CatalogManager::IsSystemTable(""));
  MT_EQ(std::string(CatalogManager::kSystemTableName), std::string("cella_catalog"));
}

MT_TEST(目录_登记与查询) {
  CatalogManager cat;  // 纯内存目录，无需存储引擎

  MT_CHECK(cat.RegisterTable(MakeTable("student")).ok());
  MT_CHECK(cat.RegisterTable(MakeTable("Course")).ok());
  MT_EQ(static_cast<int>(cat.table_count()), 2);

  // 大小写不敏感
  MT_CHECK(cat.FindTable("STUDENT") != nullptr);
  MT_CHECK(cat.FindTable("course") != nullptr);
  MT_EQ(cat.CanonicalName("cOuRsE"), std::string("Course"));
  MT_CHECK(cat.FindTable("nope") == nullptr);

  // 重名拒绝
  const DbStatus dup = cat.RegisterTable(MakeTable("STUDENT"));
  MT_CHECK(!dup.ok());
  MT_CHECK(dup.code() == DbCode::kTableExists);

  // 列查找
  const CatalogTable* t = cat.FindTable("student");
  MT_CHECK(t != nullptr);
  MT_EQ(t->ColumnIndex("ID"), 0);
  MT_EQ(t->ColumnIndex("Name"), 1);
  MT_EQ(t->ColumnIndex("zzz"), -1);
  MT_EQ(static_cast<int>(t->MaxLenAt(0)), 0);    // INT 无长度约束
  MT_EQ(static_cast<int>(t->MaxLenAt(1)), 32);

  // 表号自增且稳定
  MT_CHECK(t->table_id >= 1);
  MT_EQ(static_cast<int>(cat.ListTables().size()), 2);
}

MT_TEST(目录_删除表) {
  CatalogManager cat;
  MT_CHECK(cat.RegisterTable(MakeTable("a")).ok());
  MT_CHECK(cat.RemoveTable("A").ok());
  MT_CHECK(cat.FindTable("a") == nullptr);
  const DbStatus s = cat.RemoveTable("a");
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kTableNotFound);
}

MT_TEST(目录_编译器视图) {
  CatalogManager cat;
  MT_CHECK(cat.RegisterTable(MakeTable("student")).ok());

  const cella::CELLA_Catalog cc = cat.ToCompilerCatalog();
  const cella::CELLA_Table* t = cc.findTable("Student");
  MT_CHECK(t != nullptr);
  MT_EQ(static_cast<int>(t->columns.size()), 2);
  MT_EQ(t->columns[0].name, std::string("id"));
  MT_CHECK(t->columns[0].type == cella::CELLA_DataType::INT);
  MT_CHECK(t->columns[0].notNull);
  MT_CHECK(t->columns[1].type == cella::CELLA_DataType::VARCHAR);
  MT_EQ(t->columns[1].len, 32);
  MT_EQ(cat.DescribeTable("student").find("VARCHAR(32)") != std::string::npos, true);
}

MT_TEST(目录_缺省长度归一) {
  CatalogManager cat;
  CatalogTable t;
  t.name = "t";
  CatalogColumn c;
  c.name = "s";
  c.type = cella::CELLA_DataType::VARCHAR;
  c.len = 0;  // 未声明 → 归一为 255
  t.columns.push_back(c);
  MT_CHECK(cat.RegisterTable(t).ok());
  const CatalogTable* got = cat.FindTable("t");
  MT_CHECK(got != nullptr);
  MT_EQ(got->columns[0].len, 255);
}

MT_TEST(目录_类型名往返) {
  const cella::CELLA_DataType all[] = {
      cella::CELLA_DataType::INT,      cella::CELLA_DataType::FLOAT,
      cella::CELLA_DataType::DOUBLE,   cella::CELLA_DataType::CHAR,
      cella::CELLA_DataType::VARCHAR,  cella::CELLA_DataType::TEXT,
      cella::CELLA_DataType::DATE,     cella::CELLA_DataType::TIME,
      cella::CELLA_DataType::DATETIME};
  for (const auto t : all) {
    cella::CELLA_DataType back = cella::CELLA_DataType::INT;
    MT_CHECK(CatalogTypeFromName(CatalogTypeName(t), &back));
    MT_CHECK(back == t);
  }
  cella::CELLA_DataType unused = cella::CELLA_DataType::INT;
  MT_CHECK(!CatalogTypeFromName("BLOB", &unused));
  MT_CHECK(CatalogTypeFromName("integer", &unused));
  MT_CHECK(unused == cella::CELLA_DataType::INT);
}

MT_TEST(目录_系统表可查询) {
  Engine e("cat_query");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE student(id INT NOT NULL, name VARCHAR(32) NOT NULL);").all_ok());
  MT_CHECK(e.Run("CREATE TABLE course(id INT, title TEXT);").all_ok());

  // 目录能被 SQL 查询：只投影确定性的两列
  const ScriptReport r = e.Run("get name, columns in cella_catalog ordered name asc;");
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::ColsText(r.statements[0].result), std::string("name|columns"));
  MT_EQ(RowsText(r.statements[0].result),
        std::string("course|id INT 0 0 0, title TEXT 0 0 0\n"
                    "student|id INT 0 1 0, name VARCHAR 32 1 0"));

  // 按名字过滤取表号
  const ScriptReport r2 = e.Run("get table_id in cella_catalog limit name = 'student';");
  MT_CHECK(r2.all_ok());
  MT_EQ(RowsText(r2.statements[0].result), std::string("1"));

  // \d 视角也能看到系统表（表号 0）
  MT_CHECK(e.engine.catalog().FindTable("cella_catalog") != nullptr);
}

MT_TEST(目录_系统表写保护) {
  Engine e("cat_protect");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());

  // CREATE：语义阶段即拦（表已存在）
  const ScriptReport r1 = e.Run("CREATE TABLE cella_catalog(x INT);");
  MT_CHECK(!r1.all_ok());
  MT_CHECK(!r1.statements[0].compile_errors.empty());

  // DROP / INSERT / DELETE / UPDATE：执行器拦（DB-512）
  MT_CHECK(e.Run("DROP TABLE cella_catalog;").statements[0].status.code() ==
           DbCode::kSystemTableProtected);
  MT_CHECK(e.Run("INSERT INTO cella_catalog VALUES ('x',1,1,'y');")
               .statements[0]
               .status.code() == DbCode::kSystemTableProtected);
  MT_CHECK(e.Run("DELETE in cella_catalog limit name = 'x';")
               .statements[0]
               .status.code() == DbCode::kSystemTableProtected);
  MT_CHECK(e.Run("UPDATE cella_catalog SET name = 'z' limit name = 'x';")
               .statements[0]
               .status.code() == DbCode::kSystemTableProtected);

  // 系统表仍在，用户表未受影响
  MT_CHECK(e.engine.catalog().FindTable("cella_catalog") != nullptr);
  MT_CHECK(e.engine.catalog().FindTable("t") != nullptr);
}

MT_TEST(目录_重启后结构持久化) {
  Engine e("cat_persist");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE student(id INT NOT NULL, name VARCHAR(16) NOT NULL, "
                 "score DOUBLE, note TEXT);")
               .all_ok());
  MT_CHECK(e.Run("CREATE TABLE course(id INT, title VARCHAR(8));").all_ok());
  e.Close();
  MT_CHECK(e.Reopen());

  const CatalogTable* t = e.engine.catalog().FindTable("student");
  MT_CHECK(t != nullptr);
  MT_EQ(t->table_id, 1u);
  MT_EQ(static_cast<int>(t->columns.size()), 4);
  MT_CHECK(t->columns[0].not_null);
  MT_EQ(t->columns[1].len, 16);
  MT_CHECK(t->columns[2].type == cella::CELLA_DataType::DOUBLE);
  MT_CHECK(t->columns[3].type == cella::CELLA_DataType::TEXT);
  MT_CHECK(t->first_page_id != cella::storage::kInvalidPageId);
  MT_CHECK(t->created_at > 0);
  MT_CHECK(e.engine.catalog().FindTable("course") != nullptr);
  // 系统表仍在，表号 0
  MT_EQ(e.engine.catalog().FindTable("cella_catalog")->table_id, 0u);
}

MT_TEST(目录_单文件自包含) {
  Engine e("cat_selfcontained");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE t(id INT NOT NULL, v VARCHAR(16));"
                 "INSERT INTO t VALUES (1,'a'),(2,'b');")
               .all_ok());
  e.Close();

  // 只拷数据文件一个文件到新目录即可打开（不再依赖 catalog.meta）
  const std::string copy_dir = testutil::FreshDir("cat_selfcontained_copy");
  std::error_code ec;
  std::filesystem::copy(e.cfg.data_dir + "/" + e.cfg.db_file,
                        copy_dir + "/" + e.cfg.db_file, ec);
  MT_CHECK(!ec);

  auto e2 = OpenOn(copy_dir);
  MT_CHECK(e2 != nullptr);
  MT_CHECK(e2->catalog().FindTable("t") != nullptr);
  ScriptReport r;
  (void)e2->default_session().Execute("get id, v in t ordered id asc;", &r);
  MT_CHECK(r.all_ok());
  MT_EQ(testutil::RowsText(r.statements[0].result), std::string("1|a\n2|b"));
  e2->Close();
}

MT_TEST(目录_旧库迁移) {
  const std::string dir = testutil::FreshDir("cat_migrate");
  // 手写一份旧格式 catalog.meta
  {
    std::ofstream out(dir + "/catalog.meta", std::ios::binary);
    out << "CELLA-CATALOG 1\n"
        << "table 7 student 2 12345 1700000000\n"
        << "col id INT 0 1\n"
        << "col name VARCHAR 32 0\n"
        << "table 9 course 2 99999 1700000001\n"
        << "col id INT 0 1\n"
        << "col title VARCHAR 16 0\n";
  }
  auto e = OpenOn(dir);
  MT_CHECK(e != nullptr);
  if (e == nullptr) {
    return;
  }

  // 迁移：表结构保真（含表号、NOT NULL、长度）
  const CatalogTable* s = e->catalog().FindTable("student");
  MT_CHECK(s != nullptr);
  if (s != nullptr) {
    MT_EQ(s->table_id, 7u);
    MT_EQ(static_cast<int>(s->columns.size()), 2);
    MT_CHECK(s->columns[0].not_null);
    MT_EQ(s->columns[1].len, 32);
    MT_CHECK(s->first_page_id != cella::storage::kInvalidPageId);  // 已从存储层重取真实值
  }
  MT_CHECK(e->catalog().FindTable("course") != nullptr);

  // 旧文件改名留档，新格式不再有 catalog.meta
  MT_CHECK(!std::filesystem::exists(dir + "/catalog.meta"));
  MT_CHECK(std::filesystem::exists(dir + "/catalog.meta.migrated"));

  // 迁移后仍可继续建表（表号接续，不冲突）
  ScriptReport r;
  (void)e->default_session().Execute("CREATE TABLE extra(id INT);", &r);
  MT_CHECK(r.all_ok());
  const CatalogTable* extra = e->catalog().FindTable("extra");
  MT_CHECK(extra != nullptr);
  if (extra != nullptr) {
    MT_CHECK(extra->table_id >= 10u);
  }
  e->Close();
}

MT_TEST(目录_删数据文件后重开为空库) {
  Engine e("cat_fresh_after_delete");
  MT_CHECK(e.opened);
  MT_CHECK(e.Run("CREATE TABLE t(id INT);").all_ok());
  e.Close();

  // 删掉数据文件 → 重开得到全新空库（不再报 DB-509 或自愈）
  std::error_code ec;
  MT_CHECK(std::filesystem::remove(e.cfg.data_dir + "/" + e.cfg.db_file, ec));
  MT_CHECK(e.Reopen());
  MT_CHECK(e.engine.catalog().FindTable("t") == nullptr);
  MT_CHECK(e.engine.catalog().FindTable("cella_catalog") != nullptr);  // 系统表自动重建
  // 空库可正常建表使用
  MT_CHECK(e.Run("CREATE TABLE t2(id INT);").all_ok());
}

MT_TEST(目录_主键编码向后兼容) {
  Engine e("cat_pk_compat");
  MT_CHECK(e.opened);
  // 模拟「主键改造之前建的库」：直接用存储层建物理表 + 写一行**4 段**编码的目录行
  //（旧格式没有主键位）。新版本必须仍能解析，并把主键位视为「无主键」。
  cella::storage::Schema schema;
  schema.AddColumn("id", cella::storage::ValueType::kInt32, 0);
  MT_CHECK(e.engine.storage()->create_table("old", schema).ok());

  std::shared_ptr<cella::storage::TableHeap> heap;
  MT_CHECK(e.engine.storage()->open_table("cella_catalog", &heap).ok());
  cella::storage::Record rec;
  rec.AddValue(cella::storage::Value::Varchar("old"));
  rec.AddValue(cella::storage::Value::Int(99));
  rec.AddValue(cella::storage::Value::Int(0));
  rec.AddValue(cella::storage::Value::Varchar("id INT 0 1")); // 旧：名 类型 长度 非空
  cella::storage::Rid rid;
  MT_CHECK(heap->InsertRecord(rec, &rid).ok());

  // 关掉重开 = 模拟「旧库用新版本打开」：Open 会重载目录，会话目录也随之重建
  e.Close();
  MT_CHECK(e.Reopen());
  const CatalogTable *t = e.engine.catalog().FindTable("old");
  MT_CHECK(t != nullptr);
  MT_EQ(t->PrimaryKeyColumnIndex(), -1); // 旧行没有主键位 → 无主键
  MT_CHECK(t->columns[0].not_null);      // 其余字段照常解析
  MT_CHECK(e.Run("INSERT INTO old VALUES (1);").all_ok());
}
