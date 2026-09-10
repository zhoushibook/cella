// test_catalog.cpp —— 元数据管理测试：登记/查询/落盘/重载/与存储层互相校验。
#include <filesystem>
#include <fstream>
#include <string>

#include "cella/db/catalog/catalog_manager.h"
#include "mini_test.h"
#include "test_util.h"

using namespace cella::db;

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

}  // namespace

MT_TEST(目录_登记与查询) {
  CatalogManager cat;
  MT_CHECK(cat.Load(testutil::FreshDir("cat_reg")).ok());

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

MT_TEST(目录_落盘与重载) {
  const std::string dir = testutil::FreshDir("cat_persist");
  {
    CatalogManager cat;
    MT_CHECK(cat.Load(dir).ok());
    CatalogTable t = MakeTable("student");
    t.table_id = 7;
    t.first_page_id = 12345;
    t.created_at = 1700000000;
    MT_CHECK(cat.RegisterTable(t).ok());
    MT_CHECK(cat.RegisterTable(MakeTable("course")).ok());
    MT_CHECK(cat.Save().ok());
    MT_EQ(cat.next_table_id(), 9u);  // 第二个表分到 id=8，故下一个可用表号为 9
  }
  {
    CatalogManager cat;
    MT_CHECK(cat.Load(dir).ok());
    MT_EQ(static_cast<int>(cat.table_count()), 2);
    const CatalogTable* t = cat.FindTable("student");
    MT_CHECK(t != nullptr);
    MT_EQ(t->table_id, 7u);
    MT_EQ(static_cast<int>(t->first_page_id), 12345);
    MT_EQ(static_cast<int>(t->columns.size()), 2);
    MT_CHECK(t->columns[0].not_null);
    MT_EQ(t->columns[1].len, 32);
    MT_EQ(t->columns[1].name, std::string("name"));
    MT_EQ(cat.next_table_id(), 9u);
  }
}

MT_TEST(目录_删除表) {
  const std::string dir = testutil::FreshDir("cat_drop");
  CatalogManager cat;
  MT_CHECK(cat.Load(dir).ok());
  MT_CHECK(cat.RegisterTable(MakeTable("a")).ok());
  MT_CHECK(cat.Save().ok());
  MT_CHECK(cat.RemoveTable("A").ok());
  MT_CHECK(cat.FindTable("a") == nullptr);
  const DbStatus s = cat.RemoveTable("a");
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kTableNotFound);
}

MT_TEST(目录_文件头校验) {
  const std::string dir = testutil::FreshDir("cat_bad");
  const std::string path = dir + "/catalog.meta";
  {
    std::ofstream out(path, std::ios::binary);
    out << "WRONG-MAGIC 1\n";
  }
  CatalogManager cat;
  const DbStatus s = cat.Load(dir);
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kCatalogError);

  {
    std::ofstream out(path, std::ios::binary);
    out << "CELLA-CATALOG 99\n";
  }
  CatalogManager cat2;
  const DbStatus s2 = cat2.Load(dir);
  MT_CHECK(!s2.ok());
  MT_CHECK(s2.code() == DbCode::kCatalogError);

  // 目录不存在 → 视为空库
  CatalogManager cat3;
  MT_CHECK(cat3.Load(testutil::FreshDir("cat_empty")).ok());
  MT_EQ(static_cast<int>(cat3.table_count()), 0);
}

MT_TEST(目录_编译器视图) {
  CatalogManager cat;
  MT_CHECK(cat.Load(testutil::FreshDir("cat_compiler")).ok());
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
  MT_CHECK(cat.Load(testutil::FreshDir("cat_len")).ok());
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
  // INTEGER 是 INT 的同义词
  MT_CHECK(CatalogTypeFromName("integer", &unused));
  MT_CHECK(unused == cella::CELLA_DataType::INT);
}
