// test_common.cpp —— 公共层单元测试：类型桥 / 取值约束 / 三值逻辑比较 /
//                    结果渲染 / SQL 文本切分与事务控制识别。
#include <string>
#include <vector>

#include "cella/cella_ast.h"
#include "cella/db/common/db_status.h"
#include "cella/db/common/db_logger.h"
#include "cella/db/common/time_util.h"
#include "cella/db/common/value_bridge.h"
#include "cella/db/engine/sql_text.h"
#include "cella/db/exec/query_result.h"
#include "mini_test.h"

using namespace cella;
using namespace cella::db;

namespace {

CELLA_Expr NumLit(const std::string& text, double num) {
  CELLA_Expr e;
  e.kind = CELLA_Expr::Kind::LITERAL;
  e.lit = CELLA_LiteralKind::NUMBER;
  e.text = text;
  e.num = num;
  return e;
}

CELLA_Expr StrLit(const std::string& s) {
  CELLA_Expr e;
  e.kind = CELLA_Expr::Kind::LITERAL;
  e.lit = CELLA_LiteralKind::STRING;
  e.text = s;
  return e;
}

}  // namespace

MT_TEST(dbcode_文本) {
  MT_EQ(std::string(ToString(DbCode::kOk)), std::string("DB-000"));
  MT_EQ(std::string(ToString(DbCode::kTableNotFound)), std::string("DB-502"));
  MT_EQ(std::string(ToString(DbCode::kDeadlock)), std::string("DB-604"));
  MT_EQ(std::string(ToString(DbCode::kDivisionByZero)), std::string("DB-511"));
  const DbStatus s = DbStatus::Error(DbCode::kTableExists, "表已存在: t");
  MT_EQ(s.ToString(), std::string("[DB-503] 表已存在: t"));
  MT_CHECK(!s.ok());
  MT_CHECK(DbStatus::Ok().ok());
}

MT_TEST(类型映射) {
  MT_CHECK(ToStorageType(CELLA_DataType::INT) == storage::ValueType::kInt32);
  MT_CHECK(ToStorageType(CELLA_DataType::FLOAT) == storage::ValueType::kFloat);
  MT_CHECK(ToStorageType(CELLA_DataType::DOUBLE) == storage::ValueType::kDouble);
  MT_CHECK(ToStorageType(CELLA_DataType::VARCHAR) == storage::ValueType::kVarchar);
  MT_CHECK(ToStorageType(CELLA_DataType::TEXT) == storage::ValueType::kVarchar);
  // 存储层未实现 kDate 物理类型 → 日期族按文本存储
  MT_CHECK(ToStorageType(CELLA_DataType::DATE) == storage::ValueType::kVarchar);
  MT_CHECK(ToStorageType(CELLA_DataType::DATETIME) == storage::ValueType::kVarchar);

  MT_EQ(static_cast<int>(StorageMaxLen(CELLA_DataType::VARCHAR, 32, true)), 32);
  MT_EQ(static_cast<int>(StorageMaxLen(CELLA_DataType::VARCHAR, 0, false)), 255);
  MT_EQ(static_cast<int>(StorageMaxLen(CELLA_DataType::TEXT, 0, false)), 0);
  MT_EQ(static_cast<int>(StorageMaxLen(CELLA_DataType::INT, 0, false)), 0);
}

MT_TEST(字面量转存储值) {
  const storage::Value i = LiteralToValue(NumLit("42", 42.0));
  MT_CHECK(i.type == storage::ValueType::kInt32);
  MT_EQ(i.int32_val, 42);

  const storage::Value d = LiteralToValue(NumLit("3.5", 3.5));
  MT_CHECK(d.type == storage::ValueType::kDouble);

  const storage::Value neg = LiteralToValue(NumLit("-7", -7.0));
  MT_EQ(neg.int32_val, -7);

  const storage::Value s = LiteralToValue(StrLit("abc"));
  MT_CHECK(s.type == storage::ValueType::kVarchar);
  MT_EQ(s.str_val, std::string("abc"));

  // 整数字面量但超出 int32 → 退化为 DOUBLE，避免静默截断
  const storage::Value big = LiteralToValue(NumLit("99999999999999", 99999999999999.0));
  MT_CHECK(big.type == storage::ValueType::kDouble);

  CELLA_Expr null_lit;
  null_lit.kind = CELLA_Expr::Kind::LITERAL;
  null_lit.lit = CELLA_LiteralKind::NULL_LIT;
  MT_CHECK(LiteralToValue(null_lit).IsNull());
}

MT_TEST(取值约束_数值与文本) {
  storage::Value out;

  // 1.0 可以进 INT 列（数值族内部互转，只要值本身是整数）
  DbStatus s = CoerceValue(storage::Value::Double(1.0), CELLA_DataType::INT, 0, false, &out);
  MT_CHECK(s.ok());
  MT_EQ(out.int32_val, 1);

  // 1.5 不能进 INT 列
  s = CoerceValue(storage::Value::Double(1.5), CELLA_DataType::INT, 0, false, &out);
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kTypeMismatch);

  // 整数进 FLOAT / DOUBLE
  s = CoerceValue(storage::Value::Int(3), CELLA_DataType::DOUBLE, 0, false, &out);
  MT_CHECK(s.ok());
  MT_CHECK(out.double_val == 3.0);

  // NULL 进 NOT NULL 列
  s = CoerceValue(storage::Value::Null(), CELLA_DataType::INT, 0, true, &out);
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kNotNullViolation);

  // NULL 进可空列
  s = CoerceValue(storage::Value::Null(), CELLA_DataType::INT, 0, false, &out);
  MT_CHECK(s.ok());
  MT_CHECK(out.IsNull());

  // 文本超长
  s = CoerceValue(storage::Value::Varchar("abcdef"), CELLA_DataType::VARCHAR, 3, false, &out);
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kValueTooLong);

  // 数值写字符列 → 类型不匹配
  s = CoerceValue(storage::Value::Int(1), CELLA_DataType::VARCHAR, 8, false, &out);
  MT_CHECK(!s.ok());
  MT_CHECK(s.code() == DbCode::kTypeMismatch);

  // 日期按文本存
  s = CoerceValue(storage::Value::Varchar("2026-09-10"), CELLA_DataType::DATE, 0, false, &out);
  MT_CHECK(s.ok());
  MT_EQ(out.str_val, std::string("2026-09-10"));
}

MT_TEST(三值逻辑比较) {
  bool known = true;
  MT_EQ(CompareValues(storage::Value::Int(1), storage::Value::Int(2), &known), -1);
  MT_CHECK(known);
  MT_EQ(CompareValues(storage::Value::Double(2.0), storage::Value::Int(2), &known), 0);
  MT_CHECK(known);
  MT_EQ(CompareValues(storage::Value::Varchar("b"), storage::Value::Varchar("a"), &known), 1);
  MT_CHECK(known);

  // 与 NULL 比较 → UNKNOWN
  (void)CompareValues(storage::Value::Null(), storage::Value::Int(1), &known);
  MT_CHECK(!known);

  // 分组语义：NULL == NULL
  MT_CHECK(ValueEquals(storage::Value::Null(), storage::Value::Null()));
  MT_CHECK(!ValueEquals(storage::Value::Null(), storage::Value::Int(0)));
  MT_CHECK(ValueEquals(storage::Value::Int(5), storage::Value::Double(5.0)));
}

MT_TEST(渲染) {
  MT_EQ(RenderValue(storage::Value::Null()), std::string("NULL"));
  MT_EQ(RenderValue(storage::Value::Int(7)), std::string("7"));
  MT_EQ(RenderValue(storage::Value::Bool(true)), std::string("TRUE"));
  MT_EQ(RenderValue(storage::Value::Double(2.5)), std::string("2.5"));
  MT_EQ(RenderValue(storage::Value::Double(3.0)), std::string("3"));
}

MT_TEST(结果显示宽度) {
  MT_EQ(static_cast<int>(DisplayWidth("abc")), 3);
  MT_EQ(static_cast<int>(DisplayWidth("中文")), 4);       // 每个汉字 2 列
  MT_EQ(static_cast<int>(DisplayWidth("a中b")), 4);
  MT_EQ(static_cast<int>(DisplayWidth("")), 0);
}

MT_TEST(结果表格渲染) {
  QueryResult r;
  r.columns.push_back(ResultColumn{"id"});
  r.columns.push_back(ResultColumn{"名字"});
  r.rows.push_back({storage::Value::Int(1), storage::Value::Varchar("小明")});
  r.rows.push_back({storage::Value::Int(2), storage::Value::Null()});
  const std::string text = r.ToText();
  MT_CHECK(text.find("id") != std::string::npos);
  MT_CHECK(text.find("名字") != std::string::npos);
  MT_CHECK(text.find("NULL") != std::string::npos);
  MT_CHECK(text.find("(2 行)") != std::string::npos);
  MT_CHECK(r.IsQuery());
}

MT_TEST(SQL切分_基本) {
  const auto s = SplitSqlStatements("CREATE TABLE t(a INT);\nINSERT INTO t VALUES (1);\n");
  MT_EQ(static_cast<int>(s.size()), 2);
  MT_CHECK(s[0].terminated);
  MT_CHECK(s[1].terminated);
  MT_CHECK(s[0].text.find("CREATE") != std::string::npos);
  MT_EQ(s[0].line, 1);
}

MT_TEST(SQL切分_字符串与注释中的分号) {
  const std::string sql =
      "INSERT INTO t VALUES ('a;b');"          // 字符串里的分号不算
      "/* 块注释 ; */ INSERT INTO t VALUES (2);"
      "-- 行注释 ;\nINSERT INTO t VALUES (3);";
  const auto s = SplitSqlStatements(sql);
  MT_EQ(static_cast<int>(s.size()), 3);
  MT_CHECK(s[0].text.find("'a;b'") != std::string::npos);
}

MT_TEST(SQL切分_未闭合尾段) {
  const auto s = SplitSqlStatements("CREATE TABLE t(a INT)");
  MT_EQ(static_cast<int>(s.size()), 1);
  MT_CHECK(!s[0].terminated);
  // 纯注释/空白不入列
  MT_EQ(static_cast<int>(SplitSqlStatements("-- 只有注释\n").size()), 0);
  MT_EQ(static_cast<int>(SplitSqlStatements("   \n\t ").size()), 0);
}

MT_TEST(SQL切分_单引号转义) {
  const auto s = SplitSqlStatements("INSERT INTO t VALUES ('It''s;ok');");
  MT_EQ(static_cast<int>(s.size()), 1);
  MT_CHECK(s[0].terminated);
}

MT_TEST(事务控制语句识别) {
  std::string kw;
  MT_CHECK(IsTxnControl("BEGIN;", &kw));
  MT_EQ(kw, std::string("BEGIN"));
  MT_CHECK(IsTxnControl("  commit ;", &kw));
  MT_EQ(kw, std::string("COMMIT"));
  MT_CHECK(IsTxnControl("Rollback;", &kw));
  MT_EQ(kw, std::string("ROLLBACK"));
  MT_CHECK(IsTxnControl("START TRANSACTION;", &kw));
  MT_EQ(kw, std::string("BEGIN"));
  // 前置注释也要能识别（脚本里 BEGIN 前常带注释）
  MT_CHECK(IsTxnControl("-- 开启事务\nBEGIN;", &kw));
  MT_EQ(kw, std::string("BEGIN"));
  MT_CHECK(IsTxnControl("/* x */ END;", &kw));
  MT_EQ(kw, std::string("END"));
  // 普通语句不能误判
  MT_CHECK(!IsTxnControl("get * in t;", &kw));
  MT_CHECK(!IsTxnControl("BEGINNING;", &kw));
  MT_CHECK(!IsTxnControl("", &kw));
}

MT_TEST(时间工具) {
  const std::string dt = NowDateTime();
  MT_EQ(static_cast<int>(dt.size()), 19);  // YYYY-MM-DD HH:MM:SS
  MT_CHECK(dt.find('-') != std::string::npos);
  MT_CHECK(NowEpochSeconds() > 1600000000);
  MT_CHECK(FormatMillis(0.5).find("ms") != std::string::npos);
  MT_CHECK(FormatMillis(2500.0).find("s") != std::string::npos);
}

MT_TEST(日志门面) {
  DbLogger logger;  // 未绑定后端 → 全部丢弃，不崩
  logger.SetLevel(storage::LogLevel::kDebug);
  MT_CHECK(!logger.enabled(storage::LogLevel::kInfo));
  logger.Info(logcat::kExec, "noop");
  DbLogger::Global().Attach(nullptr);
  DbLogInfo(logcat::kEngine, "noop");
  MT_CHECK(true);
}
