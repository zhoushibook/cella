// test_sql_builder.cpp —— SQL 生成用例（方言事实见 PLAN §11）。
#include <string>

#include "cella/cella_catalog.h"
#include "cella/client/api/json.h"
#include "cella/client/api/sql_builder.h"
#include "mini_test.h"

using cella::client::JsonValue;
using cella::client::RowKey;
using cella::client::SqlLiteral;
using cella::storage::Value;

namespace {

cella::db::CatalogTable MakeTable() {
  cella::db::CatalogTable t;
  t.name = "student";
  t.table_id = 3;
  cella::db::CatalogColumn c1;
  c1.name = "id";
  c1.type = cella::CELLA_DataType::INT;
  c1.not_null = true;
  c1.primary_key = true;
  cella::db::CatalogColumn c2;
  c2.name = "name";
  c2.type = cella::CELLA_DataType::VARCHAR;
  c2.len = 16;
  cella::db::CatalogColumn c3;
  c3.name = "score";
  c3.type = cella::CELLA_DataType::DOUBLE;
  t.columns = {c1, c2, c3};
  return t;
}

}  // namespace

MT_TEST(SQL_保留字防护_标识符合法性) {
  using cella::client::ValidIdentifier;
  MT_CHECK(ValidIdentifier("student"));
  MT_CHECK(ValidIdentifier("_x1"));
  MT_CHECK(!ValidIdentifier("1x"));
  MT_CHECK(!ValidIdentifier("a-b"));
  MT_CHECK(!ValidIdentifier(""));
  MT_CHECK(!ValidIdentifier("a b"));
}

MT_TEST(SQL_字面量) {
  MT_EQ(SqlLiteral(Value::Null()), std::string("NULL"));
  MT_EQ(SqlLiteral(Value::Int(7)), std::string("7"));
  MT_EQ(SqlLiteral(Value::BigInt(393216)), std::string("393216"));
  MT_EQ(SqlLiteral(Value::Double(88.5)), std::string("88.5"));
  MT_EQ(SqlLiteral(Value::Varchar("Bob")), std::string("'Bob'"));
  MT_EQ(SqlLiteral(Value::Varchar("O'Brien")), std::string("'O''Brien'"));  // '' 转义
  MT_EQ(SqlLiteral(Value::Varchar("中文")), std::string("'中文'"));
}

MT_TEST(SQL_默认排序与分页查询) {
  const auto t = MakeTable();
  MT_EQ(cella::client::DefaultSortColumn(t), std::string("id"));  // D9：有主键 → 主键列
  cella::db::CatalogTable no_pk = t;
  for (auto& c : no_pk.columns) {
    c.primary_key = false;
  }
  MT_EQ(cella::client::DefaultSortColumn(no_pk), std::string("rowid"));

  const std::string sql = cella::client::SelectPageSql(
      "student", {"rowid", "id", "name", "score"}, "id", false, 2, 100);
  MT_EQ(sql, std::string("get rowid, id, name, score in student ordered id asc page 2, 100;"));
}

MT_TEST(SQL_定位键条件_NULL用is_null) {
  RowKey key;
  key.kind = RowKey::Kind::kFullRow;
  key.columns = {"id", "name"};
  key.values = {Value::Int(5), Value::Null()};
  MT_EQ(cella::client::KeyCondition(key), std::string("id = 5 and name is null"));

  MT_EQ(cella::client::KeyCondition(cella::client::RowidKey(196608)),
        std::string("rowid = 196608"));
}

MT_TEST(SQL_增删改语句) {
  const std::string ins = cella::client::InsertSql(
      "student", {"id", "name", "score"}, {Value::Int(1), Value::Varchar("Alice"), Value::Null()});
  MT_EQ(ins, std::string("INSERT INTO student(id, name, score) VALUES (1, 'Alice', NULL);"));

  RowKey key;
  key.kind = RowKey::Kind::kPrimary;
  key.columns = {"id"};
  key.values = {Value::Int(2)};
  const std::string upd = cella::client::UpdateSql("student", {"score"}, {Value::Double(99.5)}, key);
  MT_EQ(upd, std::string("UPDATE student SET score = 99.5 limit id = 2;"));
  MT_EQ(cella::client::DeleteSql("student", key),
        std::string("DELETE in student limit id = 2;"));

  const std::string fetch =
      cella::client::SelectByKeySql("student", {"id", "name"}, key);
  MT_EQ(fetch, std::string("get id, name in student limit id = 2;"));

  MT_EQ(cella::client::CountSql("student", "id"), std::string("get id in student;"));
}

MT_TEST(SQL_主键元组推导) {
  const auto t = MakeTable();
  RowKey key;
  MT_CHECK(cella::client::PrimaryKeyOf(t, &key));
  MT_CHECK(key.kind == RowKey::Kind::kPrimary);
  MT_EQ(static_cast<int>(key.columns.size()), 1);  // 单列主键 = 长度 1 的元组（扩展位）
  MT_EQ(key.columns[0], std::string("id"));

  cella::db::CatalogTable no_pk = t;
  for (auto& c : no_pk.columns) {
    c.primary_key = false;
  }
  MT_CHECK(!cella::client::PrimaryKeyOf(no_pk, &key));
  MT_CHECK(cella::client::PrimaryKeyOf(t, nullptr));  // 仅探测形态
}

MT_TEST(JSON_按列类型收敛单元格) {
  using cella::client::CellFromJson;
  Value v;
  std::string err;
  MT_CHECK(CellFromJson(JsonValue::Int(7), "INT", &v, &err));
  MT_CHECK(v.type == cella::storage::ValueType::kInt32);
  MT_CHECK(!CellFromJson(JsonValue::Real(7.5), "INT", &v, &err));      // INT 不要小数
  MT_CHECK(!CellFromJson(JsonValue::Str("x"), "INT", &v, &err));
  MT_CHECK(CellFromJson(JsonValue::Str("x"), "VARCHAR", &v, &err));    // 字符族要字符串
  MT_CHECK(!CellFromJson(JsonValue::Int(1), "VARCHAR", &v, &err));
  MT_CHECK(CellFromJson(JsonValue::Real(0.5), "FLOAT", &v, &err));
  MT_CHECK(v.type == cella::storage::ValueType::kFloat);
  MT_CHECK(CellFromJson(JsonValue::Int(196608), "INT64", &v, &err));   // rowid 专用通道
  MT_CHECK(v.type == cella::storage::ValueType::kInt64);
  MT_CHECK(CellFromJson(JsonValue::Null(), "VARCHAR", &v, &err));      // JSON null → NULL
  MT_CHECK(v.IsNull());
}
