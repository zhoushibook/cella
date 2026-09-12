// http_server.cpp —— 连接处理循环。
//
// 响应统一带 Connection 头；HTTP/1.1 默认 keep-alive，显式 Connection: close
// 或解析出错才收线。SO_RCVTIMEO 保证空闲连接最终被回收。
#include "cella/client/net/http_server.h"

#include <thread>

#include "cella/client/net/http_parser.h"

namespace cella::client {

namespace {

constexpr int kIdleTimeoutMs = 65 * 1000;
constexpr int kMaxRequestsPerConn = 1000;

std::string SerializeResponse(const HttpResponse& r, bool keep_alive) {
  std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + StatusText(r.status) + "\r\n";
  out += "Content-Type: " + r.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
  for (const auto& kv : r.headers) {
    out += kv.first + ": " + kv.second + "\r\n";
  }
  out += std::string("Connection: ") + (keep_alive ? "keep-alive" : "close") + "\r\n";
  out += "\r\n";
  out += r.body;
  return out;
}

}  // namespace

bool HttpServer::Start(std::uint16_t port, std::string* err) {
  if (!wsa_.ok()) {
    if (err != nullptr) {
      *err = "WSAStartup 失败";
    }
    return false;
  }
  if (!listener_.BindLocalhost(port, err)) {
    return false;
  }
  if (!listener_.Listen(64, err)) {
    return false;
  }
  port_ = port;
  return true;
}

void HttpServer::Stop() { listener_.Close(); }

void HttpServer::ServeConnection(SOCKET sock, const Handler& handler) {
  ConnSocket conn(sock);
  HttpParser parser;
  char buf[16 * 1024];

  for (int served = 0; served < kMaxRequestsPerConn; ++served) {
    while (parser.result().status == ParseStatus::kNeedMore) {
      const int n = conn.Recv(buf, sizeof buf, kIdleTimeoutMs);
      if (n <= 0) {
        return;  // 对端关闭 / 超时 / 出错
      }
      parser.Feed(buf, static_cast<size_t>(n));
    }
    const ParseResult pr = parser.result();
    if (pr.status == ParseStatus::kError) {
      const std::string out = SerializeResponse(HttpResponse::Error(pr.error_status, pr.error_text),
                                                false);
      (void)conn.SendAll(out.data(), out.size());
      return;
    }

    const HttpRequest& req = pr.req;
    HttpResponse resp = handler(req);

    const bool want_close =
        (req.Header("Connection") == "close") || (req.Header("Connection") == "Close");
    const std::string out = SerializeResponse(resp, !want_close);
    if (conn.SendAll(out.data(), out.size()) != 0) {
      return;
    }
    if (want_close) {
      return;
    }
    parser = HttpParser{};  // 同一连接上的下一个请求
  }
}

void HttpServer::Run(const Handler& handler) {
  while (listener_.valid()) {
    std::string err;
    const SOCKET sock = listener_.Accept(&err);
    if (sock == INVALID_SOCKET) {
      return;  // Stop() 被调用或监听 socket 出错
    }
    std::thread(ServeConnection, sock, handler).detach();
  }
}

}  // namespace cella::client
