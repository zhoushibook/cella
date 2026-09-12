// router.cpp —— 路由匹配实现。
#include "cella/client/api/router.h"

namespace cella::client {

std::vector<std::string> Router::Split(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') {
      ++i;
    }
    if (i >= path.size()) {
      break;
    }
    const size_t start = i;
    while (i < path.size() && path[i] != '/') {
      ++i;
    }
    out.push_back(path.substr(start, i - start));
  }
  return out;
}

void Router::Add(const std::string& method, const std::string& pattern, Handler handler) {
  Route r;
  r.method = method;
  r.segs = Split(pattern);
  r.handler = std::move(handler);
  routes_.push_back(std::move(r));
}

bool Router::Dispatch(const HttpRequest& req, HttpResponse* resp) const {
  const auto segs = Split(req.path);
  bool path_hit = false;
  for (const auto& r : routes_) {
    if (r.segs.size() != segs.size()) {
      continue;
    }
    bool ok = true;
    std::vector<std::string> params;
    for (size_t i = 0; i < r.segs.size(); ++i) {
      const std::string& pat = r.segs[i];
      if (!pat.empty() && pat.front() == '{' && pat.back() == '}') {
        params.push_back(segs[i]);  // 参数段
        continue;
      }
      if (pat != segs[i]) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    path_hit = true;
    if (r.method != req.method) {
      continue;  // 路径命中但方法不同：继续找，最后统一回 405
    }
    *resp = r.handler(req, params);
    return true;
  }
  if (path_hit) {
    *resp = HttpResponse::Error(405, "方法 " + req.method + " 不被该路径支持");
    return true;
  }
  return false;
}

}  // namespace cella::client
