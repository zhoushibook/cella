// http_server.h —— 监听 / accept / 每连接一线程 / keep-alive 循环。
//
// 线程模型（见 PLAN_web_client.md §3.3）：
//   * accept 线程阻塞等待，每来一个连接 fork 一个 worker（detach）；
//   * worker 循环「解析 → 分发 → 回写」，HTTP/1.1 默认 keep-alive，
//     空闲超时 65s 自动收线，避免线程泄漏；
//   * 业务分发交给注入的 Handler（ApiService），静态资源由 StaticFiles 处理。
#pragma once

#include <functional>
#include <string>

#include "cella/client/net/http_types.h"
#include "cella/client/net/socket.h"

namespace cella::client {

class HttpServer {
 public:
  // 返回 HttpResponse；未知路由时返回 404
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  HttpServer() = default;
  ~HttpServer() = default;
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // 绑定 + 监听。失败返回 false 并给出原因（端口占用等）。
  bool Start(std::uint16_t port, std::string* err);

  // 阻塞运行 accept 循环，直到 Stop() 被调用（如 Ctrl+C / 测试结束）。
  void Run(const Handler& handler);

  // 从其他线程安全停止（关闭监听 socket 唤醒阻塞的 accept）
  void Stop();

  std::uint16_t port() const { return port_; }

 private:
  static void ServeConnection(SOCKET sock, const Handler& handler);

  WinsockLifetime wsa_;
  ListenSocket listener_;
  std::uint16_t port_ = 0;
};

}  // namespace cella::client
