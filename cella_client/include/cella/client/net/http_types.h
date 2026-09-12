// http_types.h —— HTTP 请求/响应的数据结构（与 socket 解耦，便于单测）。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cella::client {

struct HttpRequest {
  std::string method;   // "GET" / "POST"（其余方法在上层拒绝）
  std::string target;   // 原始请求目标，如 "/api/query?x=1"
  std::string path;     // 去掉查询串后的路径（已做百分号解码）
  std::string query;    // 原始查询串（不含 '?'）
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  // 大小写不敏感取头；未命中返回空串
  std::string Header(const std::string& name) const;

  // 解析查询串为 key=value 对（已做百分号解码；'+' 视作空格）
  std::vector<std::pair<std::string, std::string>> QueryParams() const;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;  // 附加头（如 Cache-Control）

  static HttpResponse Json(int status, std::string body);
  static HttpResponse Text(int status, std::string body, const char* content_type);
  static HttpResponse Error(int status, const std::string& message);
};

const char* StatusText(int status);

// 百分号解码（'+' → 空格）；非法序列按原样保留
std::string UrlDecode(const std::string& s);

}  // namespace cella::client
