// test_http.cpp —— 请求解析用例（增量喂入 / 头部大小写 / 畸形请求 / 查询串）。
#include <string>

#include "cella/client/net/http_parser.h"
#include "mini_test.h"

using cella::client::HttpParser;
using cella::client::ParseStatus;

MT_TEST(HTTP_完整GET请求) {
  HttpParser p;
  const std::string raw =
      "GET /api/health HTTP/1.1\r\n"
      "Host: 127.0.0.1:8080\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";
  p.Feed(raw.data(), raw.size());
  MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kComplete));
  MT_EQ(p.result().req.method, std::string("GET"));
  MT_EQ(p.result().req.path, std::string("/api/health"));
  MT_EQ(p.result().req.Header("connection"), std::string("keep-alive"));
  MT_EQ(p.result().req.Header("HOST"), std::string("127.0.0.1:8080"));
  MT_CHECK(p.result().req.Header("不存在").empty());
}

MT_TEST(HTTP_增量喂入与粘包) {
  HttpParser p;
  const std::string raw =
      "POST /api/query HTTP/1.1\r\n"
      "Content-Length: 17\r\n"
      "\r\n"
      "{\"sql\":\"get id;\"}";
  p.Feed(raw.data(), 10);  // 半个请求行
  MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kNeedMore));
  p.Feed(raw.data() + 10, 20);
  MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kNeedMore));
  p.Feed(raw.data() + 30, raw.size() - 30);
  MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kComplete));
  MT_EQ(p.result().req.body, std::string("{\"sql\":\"get id;\"}"));
  MT_EQ(p.result().req.body, std::string("{\"sql\":\"get id;\"}"));
  MT_EQ(static_cast<size_t>(p.result().consumed), raw.size());
}

MT_TEST(HTTP_查询串解析与百分号解码) {
  HttpParser p;
  const std::string raw =
      "GET /api/tables/student/rows?page=2&pageSize=500&sort=score&order=desc&x=a%20b HTTP/1.1\r\n\r\n";
  p.Feed(raw.data(), raw.size());
  MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kComplete));
  const auto q = p.result().req.QueryParams();
  bool saw_page = false;
  for (const auto& kv : q) {
    if (kv.first == "page") { MT_EQ(kv.second, std::string("2")); saw_page = true; }
    if (kv.first == "pageSize") MT_EQ(kv.second, std::string("500"));
    if (kv.first == "order") MT_EQ(kv.second, std::string("desc"));
    if (kv.first == "x") MT_EQ(kv.second, std::string("a b"));
  }
  MT_CHECK(saw_page);
}

MT_TEST(HTTP_畸形请求) {
  {
    HttpParser p;
    p.Feed("garbage\r\n\r\n", 12);
    MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kError));
  }
  {
    HttpParser p;
    p.Feed("GET / HTTP/2.0\r\n\r\n", 18);
    MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kError));
    MT_EQ(p.result().error_status, 501);
  }
  {
    HttpParser p;
    p.Feed("POST /api/query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 56);
    MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kError));
    MT_EQ(p.result().error_status, 501);
  }
  {
    HttpParser p;
    p.Feed("POST /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 41);
    MT_EQ(static_cast<int>(p.result().status), static_cast<int>(ParseStatus::kError));
    MT_EQ(p.result().error_status, 400);
  }
}
