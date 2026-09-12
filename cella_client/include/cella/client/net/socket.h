// socket.h —— Winsock2 的极小 RAII 封装（Windows 专用；预留 POSIX 分支）。
//
// 只暴露服务端需要的三件事：库生命周期、监听 socket、阻塞 accept。
// 这样 HTTP/业务层完全不接触 socket API，测试可以绕开网络。
#pragma once

#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // FormatMessage/LocalFree（必须在 winsock2.h 之后，避免拉入 winsock.h）

// winnt.h 的文档宏会打碎 cella 方言的标识符（AST 的 DELETE 语句枚举、token 的 IN 关键字），
// 本模块用不到它们，在这里统一拆除 —— 这是本模块唯一的 windows.h 引入点。
#undef DELETE
#undef IN
#undef OUT
#else
// POSIX 预留：类型别名让上层代码可以原样迁移
using SOCKET = int;
constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif

namespace cella::client {

// 进程级 WSAStartup/WSACleanup（重复调用安全，引用计数）
class WinsockLifetime {
 public:
  WinsockLifetime();
  ~WinsockLifetime();
  WinsockLifetime(const WinsockLifetime&) = delete;
  WinsockLifetime& operator=(const WinsockLifetime&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};

// 监听 socket：bind 127.0.0.1 + listen，析构自动 closesocket
class ListenSocket {
 public:
  ListenSocket() = default;
  ~ListenSocket();

  ListenSocket(const ListenSocket&) = delete;
  ListenSocket& operator=(const ListenSocket&) = delete;

  // 成功返回 true；失败时 LastError() 给出可读原因
  bool BindLocalhost(std::uint16_t port, std::string* err);
  bool Listen(int backlog, std::string* err);

  // 阻塞等待一个连接；返回有效句柄或 INVALID_SOCKET（关闭/出错时）
  SOCKET Accept(std::string* err);

  // 供外部主动关闭（唤醒阻塞中的 Accept）
  void Close();
  bool valid() const { return sock_ != INVALID_SOCKET; }

  std::string LastError() const;

 private:
  SOCKET sock_ = INVALID_SOCKET;
};

// 连接级 socket：析构自动关闭
class ConnSocket {
 public:
  explicit ConnSocket(SOCKET s) : sock_(s) {}
  ~ConnSocket();
  ConnSocket(const ConnSocket&) = delete;
  ConnSocket& operator=(const ConnSocket&) = delete;

  // 收发；返回 -1 表示对端关闭/出错
  int Recv(char* buf, size_t len, int timeout_ms);
  int SendAll(const char* data, size_t len);

 private:
  SOCKET sock_;
};

}  // namespace cella::client
