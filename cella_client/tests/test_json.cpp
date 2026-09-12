// test_json.cpp —— JSON 编解码用例（中文往返 / 转义 / 精度 / 解析容错）。
#include <cmath>
#include <string>

#include "cella/client/api/json.h"
#include "mini_test.h"

using cella::client::JsonDump;
using cella::client::JsonParse;
using cella::client::JsonValue;

MT_TEST(JSON_标量与嵌套往返) {
  JsonValue v = JsonValue::Obj();
  v.Set("n", JsonValue::Null());
  v.Set("b", JsonValue::Bool(true));
  v.Set("i", JsonValue::Int(-1234567890123LL));
  v.Set("d", JsonValue::Real(88.5));
  v.Set("s", JsonValue::Str("hello"));
  JsonValue arr = JsonValue::Arr();
  arr.Push(JsonValue::Int(1));
  arr.Push(JsonValue::Str("二"));
  v.Set("a", std::move(arr));

  const std::string text = JsonDump(v);
  JsonValue back;
  std::string err;
  MT_CHECK(JsonParse(text, &back, &err));
  MT_EQ(back.Find("n")->IsNull(), true);
  MT_EQ(back.Find("b")->AsBool(), true);
  MT_EQ(back.Find("i")->AsInt(), -1234567890123LL);
  MT_CHECK(back.Find("i")->IsInt());
  MT_CHECK(std::fabs(back.Find("d")->AsDouble() - 88.5) < 1e-12);
  MT_EQ(back.Find("s")->AsString(), std::string("hello"));
  MT_EQ(static_cast<int>(back.Find("a")->size()), 2);
  MT_EQ(back.Find("a")->items()[1].AsString(), std::string("二"));
}

MT_TEST(JSON_中文与控制字符转义) {
  JsonValue v = JsonValue::Str("中文\"引号\"换行\n制表\t\x01");
  const std::string text = JsonDump(v);
  MT_CHECK(text.find("\\u0001") != std::string::npos);
  MT_CHECK(text.find("\\n") != std::string::npos);
  MT_CHECK(text.find("中文") != std::string::npos);  // UTF-8 原文输出
  JsonValue back;
  std::string err;
  MT_CHECK(JsonParse(text, &back, &err));
  MT_EQ(back.AsString(), std::string("中文\"引号\"换行\n制表\t\x01"));
}

MT_TEST(JSON_浮点精度往返) {
  for (const double x : {0.1, 1.0 / 3.0, 3.14159265358979, 1e-12, 123456.789}) {
    const std::string text = JsonDump(JsonValue::Real(x));
    JsonValue back;
    MT_CHECK(JsonParse(text, &back, nullptr));
    MT_CHECK(std::fabs(back.AsDouble() - x) == 0.0);  // 逐位还原
  }
}

MT_TEST(JSON_解析非法输入) {
  JsonValue out;
  MT_CHECK(!JsonParse("{", &out, nullptr));
  MT_CHECK(!JsonParse("[1,]", &out, nullptr));
  MT_CHECK(!JsonParse("{\"a\" 1}", &out, nullptr));
  MT_CHECK(!JsonParse("tru", &out, nullptr));
  MT_CHECK(!JsonParse("1 2", &out, nullptr));
  MT_CHECK(!JsonParse("\"未闭合", &out, nullptr));
}

MT_TEST(JSON_对象键保持插入序) {
  JsonValue v = JsonValue::Obj();
  v.Set("z", JsonValue::Int(1));
  v.Set("a", JsonValue::Int(2));
  v.Set("m", JsonValue::Int(3));
  MT_EQ(JsonDump(v), std::string("{\"z\":1,\"a\":2,\"m\":3}"));
  v.Set("z", JsonValue::Int(9));  // 已存在键原位覆盖
  MT_EQ(JsonDump(v), std::string("{\"z\":9,\"a\":2,\"m\":3}"));
}

MT_TEST(JSON_解析请求体形状) {
  const std::string body =
      R"({"sql":"get id in t;","maxRows":100,"flag":false,"plan":{"before":"a","after":"b"}})";
  JsonValue v;
  MT_CHECK(JsonParse(body, &v, nullptr));
  MT_EQ(v.Find("sql")->AsString(), std::string("get id in t;"));
  MT_EQ(v.Find("maxRows")->AsInt(), 100);
  MT_EQ(v.Find("flag")->AsBool(), false);
  MT_EQ(v.Find("plan")->Find("after")->AsString(), std::string("b"));
}
