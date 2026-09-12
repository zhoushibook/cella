// router.h —— 极简路由表：方法 + 路径模式（支持 {param} 段）。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cella/client/net/http_types.h"

namespace cella::client {

class Router {
 public:
  using Handler =
      std::function<HttpResponse(const HttpRequest& req, const std::vector<std::string>& params)>;

  // pattern 形如 "/api/catalog/{table}"；段数必须与请求路径一致
  void Add(const std::string& method, const std::string& pattern, Handler handler);

  // 未命中返回 false（调用方回 404）；路径命中但方法不符回 405
  bool Dispatch(const HttpRequest& req, HttpResponse* resp) const;

 private:
  struct Route {
    std::string method;
    std::vector<std::string> segs;  // "{name}" 表示参数段
    Handler handler;
  };

  static std::vector<std::string> Split(const std::string& path);

  std::vector<Route> routes_;
};

}  // namespace cella::client
