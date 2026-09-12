// main.cpp —— cella_web：图形客户端服务入口。
//
// 用法：
//   cella_web [选项]
// 选项：
//   --data DIR        数据目录（默认 ./cella_data）
//   --db NAME         启动库（默认 main）
//   --port N          监听端口（默认 8080；只绑 127.0.0.1）
//   --web-dir DIR     前端资源目录（默认编译期注入的 web/）
//   --page-size N     页大小（默认 4096）
//   --pool N          缓冲池帧数（默认 64）
//   --replacer NAME   替换策略 LRU|FIFO|CLOCK（默认 LRU）
//   -h, --help        显示本帮助
//
// 安全：只监听 127.0.0.1；同一数据目录只允许一个服务进程（lock 文件，PLAN §6.5）。
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "cella/client/net/http_server.h"
#include "cella/client/server/api_service.h"
#include "cella/client/server/server_config.h"

#ifdef _WIN32
// SetConsoleOutputCP / GetCurrentProcessId 由 windows.h 引入（经 http_server.h → socket.h）
#endif

namespace {

const char* const kUsage =
    "cella 数据库图形客户端（内嵌 HTTP 服务 + 浏览器单页应用）\n"
    "\n"
    "用法: cella_web [选项]\n"
    "\n"
    "选项:\n"
    "  --data DIR        数据目录（默认 ./cella_data）\n"
    "  --db NAME         启动库（默认 main）\n"
    "  --port N          监听端口（默认 8080；只绑 127.0.0.1）\n"
    "  --web-dir DIR     前端资源目录\n"
    "  --page-size N     页大小（默认 4096）\n"
    "  --pool N          缓冲池帧数（默认 64）\n"
    "  --replacer NAME   替换策略 LRU|FIFO|CLOCK（默认 LRU）\n"
    "  -h, --help        显示本帮助\n";

// 单实例锁：同一 data_dir 只允许一个服务进程（IStorage 无文件锁，PLAN §6.5）。
// 锁文件记录 pid=N；启动时校验持有者进程是否仍存活 —— 强制结束的进程不会执行析构、
// 会留下陈旧锁，这里自动识别并清理后照常接管（pid 复用导致的误判方向是保守的：拒绝启动）。
class InstanceLock {
 public:
  explicit InstanceLock(const std::string& data_dir) : path_(data_dir + "/cella-client.lock") {
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (std::filesystem::exists(path_, ec)) {
      if (HolderAlive()) {
        return;  // 真有进程在跑：拒绝启动
      }
      std::error_code rm_ec;
      std::filesystem::remove(path_, rm_ec);  // 陈旧锁：清理后接管
    }
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
      f << "pid=" << GetCurrentProcessId() << "\n";
      held_ = f.good();
    }
  }
  ~InstanceLock() {
    if (held_) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
  bool held() const { return held_; }

 private:
  // 锁文件里存在可解析的 pid=N 且该进程仍存活 → true；文件缺失/无 pid/进程已死 → false
  bool HolderAlive() const {
    std::ifstream f(path_, std::ios::binary);
    if (!f.is_open()) {
      return false;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.rfind("pid=", 0) != 0) {
        continue;
      }
      const unsigned long pid = std::strtoul(line.c_str() + 4, nullptr, 10);
      if (pid == 0) {
        return false;
      }
#if defined(_WIN32)
      HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
      if (h == nullptr) {
        return false;  // 进程已不存在
      }
      ::CloseHandle(h);
      return true;
#else
      return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
    }
    return false;
  }

  std::string path_;
  bool held_ = false;
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(65001);
  SetConsoleCP(65001);
#endif

  const cella::client::ServerConfig cfg = cella::client::ParseServerArgs(argc, argv);
  if (cfg.help) {
    std::cout << kUsage;
    return 0;
  }
  if (!cfg.error.empty()) {
    std::cerr << cfg.error << "\n" << kUsage;
    return 2;
  }

  InstanceLock lock(cfg.data_dir);
  if (!lock.held()) {
    std::cerr << "数据目录 " << cfg.data_dir
              << " 已被另一个 cella_web 进程使用（cella-client.lock 存在）。\n"
              << "同一数据目录只能有一个服务进程；如确认无进程，可手工删除该 lock 文件。\n";
    return 1;
  }

  cella::db::EngineConfig ec;
  ec.data_dir = cfg.data_dir;
  ec.db_file = cfg.db_file;
  ec.page_size = cfg.page_size;
  ec.pool_size = cfg.pool_size;
  ec.replacer = cfg.replacer;

  cella::db::DbEngine engine;
  const cella::db::DbStatus opened = engine.Open(ec);
  if (!opened.ok()) {
    std::cerr << "打开数据库失败: " << opened.ToString() << "\n";
    return 1;
  }

  cella::client::ApiService service(&engine);
  std::string web_dir = cfg.web_dir;
  if (web_dir.empty()) {
#ifdef CELLA_CLIENT_WEB_DIR
    web_dir = CELLA_CLIENT_WEB_DIR;
#else
    web_dir = "web";
#endif
  }
  service.SetWebDir(web_dir);

  cella::client::HttpServer server;
  std::string err;
  if (!server.Start(cfg.port, &err)) {
    std::cerr << "启动失败: " << err << "\n";
    engine.Close();
    return 1;
  }

  std::cout << "cella_web 已就绪\n"
            << "  地址     : http://127.0.0.1:" << cfg.port << "\n"
            << "  数据目录 : " << cfg.data_dir << "\n"
            << "  当前库   : " << engine.current_db() << "\n"
            << "  前端目录 : " << web_dir << "\n"
            << "按 Ctrl+C 停止。\n"
            << std::flush;

  // Ctrl+C：Windows 控制台默认直接终止进程（引擎在 Close 前已有存盘点语义），
  // 这里注册信号仅为了把提示打完整；优雅停机不在首版范围（PLAN §6.4）。
  server.Run([&service](const cella::client::HttpRequest& req) { return service.Handle(req); });

  engine.Close();
  return 0;
}
