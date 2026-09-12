// socket.cpp —— Winsock2 RAII 封装实现（Windows）。
#include "cella/client/net/socket.h"

#include <cstring>

namespace cella::client {

#if defined(_WIN32)

namespace {

std::string WsaMessage(int code) {
  char* msg = nullptr;
  ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(code),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   reinterpret_cast<LPSTR>(&msg), 0, nullptr);
  std::string out = "WSA 错误 " + std::to_string(code);
  if (msg != nullptr) {
    std::string text(msg);
    ::LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
      text.pop_back();
    }
    if (!text.empty()) {
      out += "：" + text;
    }
  }
  return out;
}

}  // namespace

WinsockLifetime::WinsockLifetime() {
  WSADATA data;
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

WinsockLifetime::~WinsockLifetime() {
  if (ok_) {
    ::WSACleanup();
  }
}

ListenSocket::~ListenSocket() { Close(); }

bool ListenSocket::BindLocalhost(std::uint16_t port, std::string* err) {
  sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock_ == INVALID_SOCKET) {
    if (err != nullptr) {
      *err = WsaMessage(::WSAGetLastError());
    }
    return false;
  }
  // 允许快速重启（TIME_WAIT 复用）
  BOOL reuse = TRUE;
  ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof reuse);

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    if (err != nullptr) {
      *err = "无法解析 127.0.0.1";
    }
    Close();
    return false;
  }
  if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
    if (err != nullptr) {
      const int code = ::WSAGetLastError();
      *err = (code == WSAEADDRINUSE)
                 ? "端口 " + std::to_string(port) + " 已被占用（可用 --port 换一个）"
                 : WsaMessage(code);
    }
    Close();
    return false;
  }
  return true;
}

bool ListenSocket::Listen(int backlog, std::string* err) {
  if (::listen(sock_, backlog) == SOCKET_ERROR) {
    if (err != nullptr) {
      *err = WsaMessage(::WSAGetLastError());
    }
    Close();
    return false;
  }
  return true;
}

SOCKET ListenSocket::Accept(std::string* err) {
  sockaddr_in peer;
  int len = static_cast<int>(sizeof peer);
  const SOCKET s = ::accept(sock_, reinterpret_cast<sockaddr*>(&peer), &len);
  if (s == INVALID_SOCKET && err != nullptr) {
    *err = WsaMessage(::WSAGetLastError());
  }
  return s;
}

void ListenSocket::Close() {
  if (sock_ != INVALID_SOCKET) {
    ::closesocket(sock_);
    sock_ = INVALID_SOCKET;
  }
}

std::string ListenSocket::LastError() const { return WsaMessage(::WSAGetLastError()); }

ConnSocket::~ConnSocket() {
  if (sock_ != INVALID_SOCKET) {
    ::closesocket(sock_);
  }
}

int ConnSocket::Recv(char* buf, size_t len, int timeout_ms) {
  if (timeout_ms > 0) {
    DWORD ms = static_cast<DWORD>(timeout_ms);
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
  }
  const int n = ::recv(sock_, buf, static_cast<int>(len), 0);
  return n <= 0 ? -1 : n;
}

int ConnSocket::SendAll(const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = ::send(sock_, data + sent, static_cast<int>(len - sent), 0);
    if (n <= 0) {
      return -1;
    }
    sent += static_cast<size_t>(n);
  }
  return 0;
}

#else  // POSIX 预留分支（当前不构建，保证接口可迁移）

WinsockLifetime::WinsockLifetime() : ok_(true) {}
WinsockLifetime::~WinsockLifetime() = default;
ListenSocket::~ListenSocket() { Close(); }
bool ListenSocket::BindLocalhost(std::uint16_t port, std::string* err) {
  (void)port;
  if (err != nullptr) *err = "POSIX 分支未实现";
  return false;
}
bool ListenSocket::Listen(int, std::string* err) {
  if (err != nullptr) *err = "POSIX 分支未实现";
  return false;
}
SOCKET ListenSocket::Accept(std::string* err) {
  if (err != nullptr) *err = "POSIX 分支未实现";
  return INVALID_SOCKET;
}
void ListenSocket::Close() { sock_ = INVALID_SOCKET; }
std::string ListenSocket::LastError() const { return "POSIX 分支未实现"; }
ConnSocket::~ConnSocket() = default;
int ConnSocket::Recv(char*, size_t, int) { return -1; }
int ConnSocket::SendAll(const char*, size_t) { return -1; }

#endif

}  // namespace cella::client
