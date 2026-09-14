// api_service.h —— REST 端点集合（不依赖 socket，便于单测）。
//
// 线程模型（PLAN §3.3）：
//   * `gate_`（engine_gate_）串行化全部引擎调用 —— 与 DbEngine::storage_mutex_
//     同构且锁序一致（gate_ → storage_mutex_），不会引入新的死锁路径；
//   * 静态资源与 JSON 编解码在锁外，可并行。
//
// 设计约束：本类不接触任何 socket。HTTP 服务器只是它的驱动，
// 测试可以直接构造 HttpRequest 调 Handle()。
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cella/client/api/error_map.h"
#include "cella/client/api/json.h"
#include "cella/client/api/router.h"
#include "cella/client/api/sql_builder.h"
#include "cella/client/net/http_types.h"
#include "cella/client/net/static_files.h"
#include "cella/db/engine/db_engine.h"

namespace cella::client
{

  class ApiService
  {
  public:
    explicit ApiService(cella::db::DbEngine *engine);

    // web/ 资源目录（绝对路径）；空串表示不提供静态文件
    void SetWebDir(const std::string &dir);

    // socket 无关的总入口
    HttpResponse Handle(const HttpRequest &req);

  private:
    // ── 端点（均已持有 gate_ 或无需引擎访问）──────────────────
    HttpResponse Health(const HttpRequest &req, const std::vector<std::string> &p);
    // 登录 / 登出（公开端点：登录前当然没有令牌）
    HttpResponse Login(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Logout(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Databases(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse UseDatabase(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse CreateDatabase(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse DropDatabase(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Query(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Plan(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse SessionInfo(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Txn(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Checkpoint(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Diagnostics(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Catalog(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse CatalogOne(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse Rows(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse RowCount(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse InsertRow(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse UpdateRow(const HttpRequest &req, const std::vector<std::string> &p);
    HttpResponse DeleteRow(const HttpRequest &req, const std::vector<std::string> &p);

    // PATCH/DELETE 共用主体（乐观校验流程）
    HttpResponse EditRow(const HttpRequest &req, const std::vector<std::string> &params,
                         bool is_delete);

    // ── 访问控制（令牌表；均需已持有 gate_）──────────────────
    // 一张令牌 = 一个已认证身份 + 过期时刻。过期/未知一律视为未登录。
    struct TokenInfo
    {
      std::string user;
      bool is_admin = false;
      std::chrono::steady_clock::time_point expires;
      std::shared_ptr<cella::db::Session> session;
    };
    std::string IssueToken(std::string user, bool is_admin);
    // 从 Authorization: Bearer <token>（或 ?token=）取出并校验；未命中返回 nullptr
    const TokenInfo *LookupToken(const HttpRequest &req);
    void RevokeToken(const HttpRequest &req);

    // 当前请求会话：认证令牌使用独立 Session，未认证请求回退到 CLI 默认会话。
    cella::db::Session &RequestSession();

    // ── 内部工具（需已持有 gate_）─────────────────────────────
    const cella::db::CatalogTable *FindTableLocked(const std::string &name);
    JsonValue TableJsonLocked(const cella::db::CatalogTable &t);
    JsonValue ColumnsJsonLocked(const cella::db::QueryResult &qr);
    JsonValue RowsJsonLocked(const cella::db::QueryResult &qr, size_t max_rows, bool *truncated);

    // 定位键 JSON → RowKey（按目录列类型收敛值）
    bool KeyFromJsonLocked(const cella::db::CatalogTable &table, const JsonValue &j, RowKey *key,
                           std::string *err);

    cella::db::DbEngine *engine_;
    // engine_gate_：引擎调用串行化。**递归**互斥量 —— Handle() 先在同一临界区内
    // 完成「校验令牌 + 注入身份」，再调用处理器（处理器自己也会锁同一把锁）。
    std::recursive_mutex gate_;
    std::map<std::string, TokenInfo> tokens_; // 令牌表（同样由 gate_ 保护）
    cella::db::Session *request_session_ = nullptr;
    std::unique_ptr<StaticFiles> static_;
    Router router_;
    std::chrono::steady_clock::time_point started_;
  };

} // namespace cella::client
