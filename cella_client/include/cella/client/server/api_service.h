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

namespace cella::client {

class ApiService {
 public:
  explicit ApiService(cella::db::DbEngine* engine);

  // web/ 资源目录（绝对路径）；空串表示不提供静态文件
  void SetWebDir(const std::string& dir);

  // socket 无关的总入口
  HttpResponse Handle(const HttpRequest& req);

 private:
  // ── 端点（均已持有 gate_ 或无需引擎访问）──────────────────
  HttpResponse Health(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Databases(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse UseDatabase(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse CreateDatabase(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse DropDatabase(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Query(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Plan(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse SessionInfo(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Txn(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Checkpoint(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Diagnostics(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Catalog(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse CatalogOne(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse Rows(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse RowCount(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse InsertRow(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse UpdateRow(const HttpRequest& req, const std::vector<std::string>& p);
  HttpResponse DeleteRow(const HttpRequest& req, const std::vector<std::string>& p);

  // PATCH/DELETE 共用主体（乐观校验流程）
  HttpResponse EditRow(const HttpRequest& req, const std::vector<std::string>& params,
                       bool is_delete);

  // ── 内部工具（需已持有 gate_）─────────────────────────────
  const cella::db::CatalogTable* FindTableLocked(const std::string& name);
  JsonValue TableJsonLocked(const cella::db::CatalogTable& t);
  JsonValue ColumnsJsonLocked(const cella::db::QueryResult& qr);
  JsonValue RowsJsonLocked(const cella::db::QueryResult& qr, size_t max_rows, bool* truncated);

  // 定位键 JSON → RowKey（按目录列类型收敛值）
  bool KeyFromJsonLocked(const cella::db::CatalogTable& table, const JsonValue& j, RowKey* key,
                         std::string* err);

  cella::db::DbEngine* engine_;
  std::mutex gate_;                 // engine_gate_：引擎调用串行化
  std::unique_ptr<StaticFiles> static_;
  Router router_;
  std::chrono::steady_clock::time_point started_;
};

}  // namespace cella::client
