// api_service.cpp —— REST 端点实现。
//
// 统一约定（PLAN §4.3）：
//   成功 { ok:true,  data:{...} }；失败 { ok:false, error:{code,message,line,col,detail} }
//
// 乐观校验（PLAN §6.1 + §11.4）：保存 = 事务内「按定位键重取该行 → 与界面旧值比对 →
// 一致才写」。陈旧 rowid 与不存在的 rowid 都返回空结果、分不清「被改」与「被删」，
// 统一提示「该行已不存在或已被修改」（不假装能区分）。
#include "cella/client/server/api_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>

#include "cella/db/common/value_bridge.h" // RenderValue / ValueEquals

namespace cella::client
{

  namespace fs = std::filesystem;

  using cella::db::CatalogColumn;
  using cella::db::CatalogTable;
  using cella::db::DbStatus;
  using cella::db::QueryResult;
  using cella::db::ScriptReport;
  using cella::db::Session;
  using cella::db::StatementOutcome;
  using cella::storage::Value;

  namespace
  {

    // 令牌有效期：12 小时（本地开发工具，够用且不至于长期有效）
    constexpr int kTokenTtlSeconds = 12 * 60 * 60;

    // 错误码 → HTTP 状态：未登录 401、权限不足 403，其余仍是 400
    int HttpStatusFor(cella::db::DbCode c)
    {
      switch (c)
      {
      case cella::db::DbCode::kNoCredentials:
        return 401;
      case cella::db::DbCode::kPermissionDenied:
        return 403;
      default:
        return 400;
      }
    }

    std::string RandomToken()
    {
      static const char kHex[] = "0123456789abcdef";
      std::random_device rd;
      std::mt19937_64 gen(rd());
      std::uniform_int_distribution<int> dist(0, 15);
      std::string s;
      s.reserve(32);
      for (int i = 0; i < 32; ++i)
      {
        s += kHex[dist(gen)];
      }
      return s;
    }

    constexpr size_t kDefaultMaxRows = 5000;
    constexpr size_t kHardMaxRows = 200000;
    constexpr int kDefaultPageSize = 100;
    constexpr int kMaxPageSize = 100000;
    const char *const kRowGone = "该行已不存在或已被修改，已为您刷新";

    HttpResponse OkResponse(JsonValue data)
    {
      JsonValue root = JsonValue::Obj();
      root.Set("ok", JsonValue::Bool(true));
      root.Set("data", std::move(data));
      return HttpResponse::Json(200, JsonDump(root));
    }

    HttpResponse FailResponse(int status, const std::string &code, const std::string &message,
                              const std::string &detail = std::string())
    {
      return HttpResponse::Json(status, JsonDump(ErrorBody(code, message, 0, 0, detail)));
    }

    HttpResponse BodyError(const std::string &why)
    {
      return FailResponse(400, "HTTP-400", why);
    }

    bool ParseBody(const HttpRequest &req, JsonValue *out, HttpResponse *err_resp)
    {
      if (req.body.empty())
      {
        *err_resp = BodyError("请求体不能为空");
        return false;
      }
      std::string why;
      if (!JsonParse(req.body, out, &why))
      {
        *err_resp = BodyError("请求体不是合法 JSON：" + why);
        return false;
      }
      if (!out->IsObject())
      {
        *err_resp = BodyError("请求体应为 JSON 对象");
        return false;
      }
      return true;
    }

    std::string BodyString(const JsonValue &body, const std::string &key, const std::string &def)
    {
      const JsonValue *v = body.Find(key);
      return (v != nullptr && v->IsString()) ? v->AsString() : def;
    }

    // storage::Value → JSON（类型保真：NULL→null，数值→number，字符串→string）
    JsonValue ValueToJson(const cella::storage::Value &v)
    {
      using cella::storage::ValueType;
      switch (v.type)
      {
      case ValueType::kNull:
        return JsonValue::Null();
      case ValueType::kBool:
        return JsonValue::Bool(v.bool_val);
      case ValueType::kInt32:
        return JsonValue::Int(v.int32_val);
      case ValueType::kInt64:
        return JsonValue::Int(v.int64_val);
      case ValueType::kFloat:
        return JsonValue::Real(v.float_val);
      case ValueType::kDouble:
        return JsonValue::Real(v.double_val);
      default:
        return JsonValue::Str(v.str_val);
      }
    }

    // 列类型推断：首个非 NULL 值的存储类型；全 NULL → "NULL"（PLAN §4.3）
    std::string InferColumnType(const QueryResult &qr, size_t col)
    {
      for (const auto &row : qr.rows)
      {
        if (col < row.size() && !row[col].IsNull())
        {
          return cella::storage::ToString(row[col].type);
        }
      }
      return "NULL";
    }

    const char *DiagTitle(const std::string &kind)
    {
      if (kind == "stats")
        return "缓冲池统计";
      if (kind == "locks")
        return "锁表";
      if (kind == "waitfor")
        return "等待图";
      if (kind == "txn")
        return "事务表";
      return kind.c_str();
    }

    // 定位键的列类型：rowid 伪列不在目录里，按 INT64 处理
    std::string KeyColumnDeclType(const CatalogTable &t, const std::string &col)
    {
      if (col == "rowid")
      {
        return "INT64";
      }
      for (const auto &c : t.columns)
      {
        if (c.name == col)
        {
          return cella::db::CatalogTypeName(c.type);
        }
      }
      return std::string();
    }

    // 令牌来源：`Authorization: Bearer <token>`，或 `Cookie: cella_token=<token>`。
    // 前者给 fetch 用，后者预留（浏览器直接下载静态资源时也能带上）。
    std::string TokenFromRequest(const HttpRequest &req)
    {
      const std::string auth = req.Header("Authorization");
      const std::string bearer = "Bearer ";
      if (auth.size() > bearer.size() && auth.compare(0, bearer.size(), bearer) == 0)
      {
        return auth.substr(bearer.size());
      }
      const std::string cookie = req.Header("Cookie");
      const std::string key = "cella_token=";
      const size_t pos = cookie.find(key);
      if (pos != std::string::npos)
      {
        const size_t begin = pos + key.size();
        size_t end = cookie.find(';', begin);
        if (end == std::string::npos)
        {
          end = cookie.size();
        }
        return cookie.substr(begin, end - begin);
      }
      return std::string();
    }

  } // namespace

  ApiService::ApiService(cella::db::DbEngine *engine)
      : engine_(engine),
        static_(std::make_unique<StaticFiles>(std::string())),
        started_(std::chrono::steady_clock::now())
  {
    Router &r = router_;
    auto bind = [&](const char *m, const char *pat,
                    HttpResponse (ApiService::*fn)(const HttpRequest &, const std::vector<std::string> &))
    {
      ApiService *self = this;
      r.Add(m, pat, [self, fn](const HttpRequest &q, const std::vector<std::string> &p)
            { return (self->*fn)(q, p); });
    };
    bind("GET", "/api/health", &ApiService::Health);
    bind("POST", "/api/login", &ApiService::Login);
    bind("POST", "/api/logout", &ApiService::Logout);
    bind("GET", "/api/databases", &ApiService::Databases);
    bind("POST", "/api/databases/use", &ApiService::UseDatabase);
    bind("POST", "/api/databases/create", &ApiService::CreateDatabase);
    bind("POST", "/api/databases/drop", &ApiService::DropDatabase);
    bind("POST", "/api/query", &ApiService::Query);
    bind("POST", "/api/plan", &ApiService::Plan);
    bind("GET", "/api/session", &ApiService::SessionInfo);
    bind("POST", "/api/txn/{op}", &ApiService::Txn);
    bind("POST", "/api/checkpoint", &ApiService::Checkpoint);
    bind("GET", "/api/diagnostics/{kind}", &ApiService::Diagnostics);
    bind("GET", "/api/catalog", &ApiService::Catalog);
    bind("GET", "/api/catalog/{table}", &ApiService::CatalogOne);
    bind("GET", "/api/tables/{table}/rows", &ApiService::Rows);
    bind("GET", "/api/tables/{table}/count", &ApiService::RowCount);
    bind("POST", "/api/tables/{table}/rows", &ApiService::InsertRow);
    bind("PATCH", "/api/tables/{table}/rows", &ApiService::UpdateRow);
    bind("DELETE", "/api/tables/{table}/rows", &ApiService::DeleteRow);
  }

  void ApiService::SetWebDir(const std::string &dir)
  {
    static_ = std::make_unique<StaticFiles>(dir);
  }

  HttpResponse ApiService::Handle(const HttpRequest &req)
  {
    const bool is_api = (req.path.rfind("/api/", 0) == 0 || req.path == "/api");
    if (!is_api)
    {
      HttpResponse resp;
      if (!static_->TryServe(req.path.empty() ? "/" : req.path, &resp))
      {
        resp = HttpResponse::Error(404, "资源不存在");
      }
      return resp;
    }

    // 访问控制：/api/health 与 /api/login 公开（前者要让前端知道「是否需要登录」），
    // 其余端点一律要求有效令牌。校验与**身份注入必须在同一个临界区内**完成 ——
    // 身份存在共享会话上，若在锁外注入会被并发请求互相踩。
    std::lock_guard<std::recursive_mutex> lk(gate_);
    request_session_ = &engine_->default_session();
    const bool public_route =
        (req.path == "/api/health" || req.path == "/api/login" || req.path == "/api/logout");
    if (engine_->auth_enabled())
    {
      const TokenInfo *tok = LookupToken(req); // 公开端点也允许带令牌（用于显示身份）
      if (tok != nullptr)
      {
        request_session_ = tok->session.get();
      }
      else
      {
        // 未登录：清空身份，避免上一个请求的残留身份泄漏到本次响应
        request_session_ = &engine_->default_session();
        request_session_->SetIdentity(std::string(), false);
        if (!public_route)
        {
          request_session_ = nullptr;
          return FailResponse(401, "HTTP-401", "未登录或登录已过期，请重新登录");
        }
      }
    }

    HttpResponse resp;
    if (!router_.Dispatch(req, &resp))
    {
      resp = HttpResponse::Error(404, "未知接口: " + req.path);
    }
    request_session_ = nullptr;
    return resp;
  }

  // ── 令牌与登录 ──────────────────────────────────────────────

  std::string ApiService::IssueToken(std::string user, bool is_admin)
  {
    const std::string token = RandomToken();
    TokenInfo info;
    info.user = std::move(user);
    info.is_admin = is_admin;
    info.expires = std::chrono::steady_clock::now() + std::chrono::seconds(kTokenTtlSeconds);
    info.session = std::make_shared<Session>(engine_, "web:" + token.substr(0, 8));
    info.session->SetIdentity(info.user, info.is_admin);
    tokens_[token] = std::move(info);
    return token;
  }

  Session &ApiService::RequestSession()
  {
    return request_session_ != nullptr ? *request_session_ : engine_->default_session();
  }

  const ApiService::TokenInfo *ApiService::LookupToken(const HttpRequest &req)
  {
    const std::string token = TokenFromRequest(req);
    if (token.empty())
    {
      return nullptr;
    }
    const auto it = tokens_.find(token);
    if (it == tokens_.end())
    {
      return nullptr;
    }
    if (it->second.expires <= std::chrono::steady_clock::now())
    {
      tokens_.erase(it); // 释放绑定的 Session，析构时回滚未提交事务
      return nullptr;    // 过期即视为未登录
    }
    return &it->second;
  }

  void ApiService::RevokeToken(const HttpRequest &req)
  {
    const std::string token = TokenFromRequest(req);
    if (!token.empty())
    {
      tokens_.erase(token);
    }
  }

  HttpResponse ApiService::Login(const HttpRequest &req, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    if (!engine_->auth_enabled())
    {
      return FailResponse(400, "HTTP-400", "服务端未启用访问控制（启动时加 --auth）");
    }
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    const std::string user = BodyString(body, "user", std::string());
    const std::string password = BodyString(body, "password", std::string());
    if (user.empty())
    {
      return BodyError("用户名不能为空");
    }
    bool is_admin = false;
    const DbStatus st = engine_->Authenticate(user, password, &is_admin);
    if (!st.ok())
    {
      return FailResponse(401, cella::db::ToString(st.code()), st.message());
    }
    const std::string token = IssueToken(user, is_admin);
    JsonValue data = JsonValue::Obj();
    data.Set("token", JsonValue::Str(token));
    data.Set("user", JsonValue::Str(user));
    data.Set("admin", JsonValue::Bool(is_admin));
    data.Set("expiresIn", JsonValue::Int(kTokenTtlSeconds));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::Logout(const HttpRequest &req, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    RevokeToken(req);
    JsonValue data = JsonValue::Obj();
    data.Set("note", JsonValue::Str("已登出"));
    return OkResponse(std::move(data));
  }

  // ── 元信息 ──────────────────────────────────────────────────

  HttpResponse ApiService::Health(const HttpRequest &, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    const auto &cfg = engine_->config();
    JsonValue data = JsonValue::Obj();
    data.Set("version", JsonValue::Str("cella-web/0.1"));
    data.Set("opened", JsonValue::Bool(engine_->opened()));
    data.Set("currentDb", JsonValue::Str(engine_->current_db()));
    data.Set("dataDir", JsonValue::Str(cfg.data_dir));
    data.Set("pageSize", JsonValue::Int(cfg.page_size));
    data.Set("poolSize", JsonValue::Int(static_cast<std::int64_t>(cfg.pool_size)));
    data.Set("replacer", JsonValue::Str(cfg.replacer));
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_);
    data.Set("uptimeSeconds", JsonValue::Int(secs.count()));
    // 访问控制：前端据此决定是否弹登录框；已登录时顺带给出身份
    const Session &s = RequestSession();
    data.Set("authEnabled", JsonValue::Bool(engine_->auth_enabled()));
    data.Set("user", JsonValue::Str(s.user()));
    data.Set("admin", JsonValue::Bool(!s.user().empty() && s.is_admin()));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::Databases(const HttpRequest &, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    QueryResult qr;
    const DbStatus st = engine_->ShowDatabases(&qr);
    if (!st.ok())
    {
      return FailResponse(500, cella::db::ToString(st.code()), st.message());
    }
    // 与 SQL 路径一致：普通用户只列有权限的库
    RequestSession().FilterDatabasesByPrivilege(&qr);
    const std::string &current = engine_->current_db();
    const std::string &dir = engine_->config().data_dir;
    JsonValue arr = JsonValue::Arr();
    for (const auto &row : qr.rows)
    {
      if (row.empty())
      {
        continue;
      }
      const std::string name = cella::db::RenderValue(row[0]);
      JsonValue item = JsonValue::Obj();
      item.Set("name", JsonValue::Str(name));
      item.Set("current", JsonValue::Bool(name == current));
      std::error_code ec;
      const auto size = fs::file_size(fs::path(dir) / (name + ".db"), ec);
      item.Set("sizeBytes", JsonValue::Int(ec ? 0 : static_cast<std::int64_t>(size)));
      arr.Push(std::move(item));
    }
    JsonValue data = JsonValue::Obj();
    data.Set("current", JsonValue::Str(current));
    data.Set("databases", std::move(arr));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::UseDatabase(const HttpRequest &req, const std::vector<std::string> &)
  {
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    const std::string name = BodyString(body, "name", std::string());
    if (!ValidIdentifier(name))
    {
      return BodyError("库名非法: " + name);
    }
    std::lock_guard<std::recursive_mutex> lk(gate_);
    // 访问控制：库级动作必须走与 SQL 路径同一套判定（否则会绕过权限）
    DbStatus st = RequestSession().CheckDatabaseAccess(name);
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    std::string note;
    st = engine_->UseDatabase(name, &note);
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    JsonValue data = JsonValue::Obj();
    data.Set("current", JsonValue::Str(engine_->current_db()));
    data.Set("note", JsonValue::Str(note));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::CreateDatabase(const HttpRequest &req, const std::vector<std::string> &)
  {
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    const std::string name = BodyString(body, "name", std::string());
    if (!ValidIdentifier(name))
    {
      return BodyError("库名非法: " + name);
    }
    std::lock_guard<std::recursive_mutex> lk(gate_);
    DbStatus st = RequestSession().RequireAdmin("CREATE DATABASE");
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    std::string note;
    st = engine_->CreateDatabase(name, &note);
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    JsonValue data = JsonValue::Obj();
    data.Set("name", JsonValue::Str(name));
    data.Set("note", JsonValue::Str(note));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::DropDatabase(const HttpRequest &req, const std::vector<std::string> &)
  {
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    const std::string name = BodyString(body, "name", std::string());
    if (!ValidIdentifier(name))
    {
      return BodyError("库名非法: " + name);
    }
    std::lock_guard<std::recursive_mutex> lk(gate_);
    DbStatus st = RequestSession().RequireAdmin("DROP DATABASE");
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    std::string note;
    st = engine_->DropDatabase(name, &note);
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    JsonValue data = JsonValue::Obj();
    data.Set("name", JsonValue::Str(name));
    data.Set("note", JsonValue::Str(note));
    return OkResponse(std::move(data));
  }

  // ── SQL 执行 ────────────────────────────────────────────────

  HttpResponse ApiService::Query(const HttpRequest &req, const std::vector<std::string> &)
  {
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    const std::string sql = BodyString(body, "sql", std::string());
    if (sql.empty())
    {
      return BodyError("sql 不能为空");
    }
    const JsonValue *mr = body.Find("maxRows");
    size_t max_rows = kDefaultMaxRows;
    if (mr != nullptr && mr->IsNumber())
    {
      max_rows = static_cast<size_t>(std::max<std::int64_t>(1, mr->AsInt()));
    }
    max_rows = std::min(max_rows, kHardMaxRows);

    // 服务端墙钟：从进入处理器（含排队等 gate_ 引擎锁）到结果 JSON 编码完成。
    // 与前端测得的「端到端」之差 ≈ HTTP + 网络 + 浏览器解析开销，
    // 两者都报出来才能区分「查询慢」与「传得慢」。
    const auto t_req = std::chrono::steady_clock::now();

    std::lock_guard<std::recursive_mutex> lk(gate_);
    ScriptReport report;
    const DbStatus st = RequestSession().Execute(sql, &report);

    JsonValue stmts = JsonValue::Arr();
    std::int64_t index = 0;
    for (const auto &s : report.statements)
    {
      ++index;
      const bool ok = s.status.ok() && s.compile_errors.empty();
      JsonValue j = JsonValue::Obj();
      j.Set("index", JsonValue::Int(index));
      j.Set("sql", JsonValue::Str(s.sql));
      j.Set("kind", JsonValue::Str(s.kind));
      j.Set("line", JsonValue::Int(s.line));
      j.Set("col", JsonValue::Int(s.col));
      j.Set("ok", JsonValue::Bool(ok));
      j.Set("executed", JsonValue::Bool(s.executed));
      j.Set("autoCommitted", JsonValue::Bool(s.auto_committed));
      j.Set("implicitCommit", JsonValue::Bool(s.implicit_commit));
      j.Set("rolledBackHere", JsonValue::Bool(s.rolled_back_here));
      j.Set("notice", JsonValue::Str(s.notice));
      j.Set("elapsedMs", JsonValue::Real(s.elapsed_ms));
      j.Set("operatorCalls", JsonValue::Int(static_cast<std::int64_t>(s.operator_calls)));
      j.Set("txnId", JsonValue::Int(s.txn_id));
      j.Set("affected", JsonValue::Int(static_cast<std::int64_t>(s.result.affected)));
      j.Set("tag", JsonValue::Str(s.result.tag));
      // 未截断的结果行数。rows 已被 maxRows 砍过，前端拿它的长度做「行数 × 耗时」
      // 对比会低估；这里报引擎真正产出的行数。
      j.Set("rowCount", JsonValue::Int(static_cast<std::int64_t>(s.result.rows.size())));
      if (s.result.IsQuery())
      {
        bool truncated = false;
        j.Set("columns", ColumnsJsonLocked(s.result));
        j.Set("rows", RowsJsonLocked(s.result, max_rows, &truncated));
        j.Set("truncated", JsonValue::Bool(truncated));
      }
      else
      {
        j.Set("columns", JsonValue::Null());
        j.Set("rows", JsonValue::Null());
        j.Set("truncated", JsonValue::Bool(false));
      }
      j.Set("error", StatementErrorJson(s));
      if (!s.plan_text.empty() || !s.original_plan_text.empty())
      {
        JsonValue plan = JsonValue::Obj();
        plan.Set("before", JsonValue::Str(s.original_plan_text));
        plan.Set("after", JsonValue::Str(s.plan_text));
        j.Set("plan", std::move(plan));
      }
      else
      {
        j.Set("plan", JsonValue::Null());
      }
      stmts.Push(std::move(j));
    }

    Session &session = RequestSession();
    JsonValue data = JsonValue::Obj();
    data.Set("statements", std::move(stmts));
    JsonValue sess = JsonValue::Obj();
    sess.Set("inTxn", JsonValue::Bool(session.in_transaction()));
    sess.Set("txnId", JsonValue::Int(session.current_txn()));
    data.Set("session", std::move(sess));
    data.Set("engineOk", JsonValue::Bool(st.ok()));
    // 服务端处理总耗时（含引擎排队）。语句级 elapsedMs 只覆盖引擎内的编译 + 执行，
    // 二者差额就是排队与结果编码的时间。
    data.Set("serverMs",
             JsonValue::Real(std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t_req)
                                 .count()));
    JsonValue root = JsonValue::Obj();
    root.Set("ok", JsonValue::Bool(true));
    root.Set("data", std::move(data));
    const std::string dumped = JsonDump(root);
    return HttpResponse::Json(200, std::move(dumped));
  }

  HttpResponse ApiService::Plan(const HttpRequest &req, const std::vector<std::string> &)
  {
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    const std::string sql = BodyString(body, "sql", std::string());
    if (sql.empty())
    {
      return BodyError("sql 不能为空");
    }
    std::lock_guard<std::recursive_mutex> lk(gate_);
    ScriptReport report;
    const auto t0 = std::chrono::steady_clock::now();
    (void)RequestSession().CompileOnly(sql, &report);
    const double compile_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    JsonValue stmts = JsonValue::Arr();
    std::int64_t index = 0;
    for (const auto &s : report.statements)
    {
      ++index;
      const bool ok = s.status.ok() && s.compile_errors.empty();
      JsonValue j = JsonValue::Obj();
      j.Set("index", JsonValue::Int(index));
      j.Set("sql", JsonValue::Str(s.sql));
      j.Set("kind", JsonValue::Str(s.kind));
      j.Set("ok", JsonValue::Bool(ok));
      j.Set("error", StatementErrorJson(s));
      JsonValue plan = JsonValue::Obj();
      plan.Set("before", JsonValue::Str(s.original_plan_text));
      plan.Set("after", JsonValue::Str(s.plan_text));
      j.Set("plan", std::move(plan));
      stmts.Push(std::move(j));
    }
    JsonValue data = JsonValue::Obj();
    data.Set("statements", std::move(stmts));
    data.Set("elapsedMs", JsonValue::Real(compile_ms)); // 编译（含优化）耗时
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::SessionInfo(const HttpRequest &, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    Session &s = RequestSession();
    JsonValue data = JsonValue::Obj();
    data.Set("inTxn", JsonValue::Bool(s.in_transaction()));
    data.Set("txnId", JsonValue::Int(s.current_txn()));
    data.Set("currentDb", JsonValue::Str(engine_->current_db()));
    data.Set("statusLine", JsonValue::Str(s.StatusLine()));
    data.Set("authEnabled", JsonValue::Bool(engine_->auth_enabled()));
    data.Set("user", JsonValue::Str(s.user()));
    data.Set("admin", JsonValue::Bool(!s.user().empty() && s.is_admin()));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::Txn(const HttpRequest &, const std::vector<std::string> &params)
  {
    const std::string op = params.empty() ? std::string() : params[0];
    std::lock_guard<std::recursive_mutex> lk(gate_);
    Session &s = RequestSession();
    std::string note;
    DbStatus st;
    if (op == "begin")
    {
      st = s.Begin(&note);
    }
    else if (op == "commit")
    {
      st = s.Commit(&note);
    }
    else if (op == "rollback")
    {
      st = s.Rollback(&note);
    }
    else
    {
      return HttpResponse::Error(404, "未知事务操作: " + op);
    }
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    JsonValue data = JsonValue::Obj();
    data.Set("op", JsonValue::Str(op));
    data.Set("note", JsonValue::Str(note));
    data.Set("inTxn", JsonValue::Bool(s.in_transaction()));
    data.Set("txnId", JsonValue::Int(s.current_txn()));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::Checkpoint(const HttpRequest &, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    const DbStatus perm = RequestSession().RequireAdmin("存盘点");
    if (!perm.ok())
    {
      return FailResponse(HttpStatusFor(perm.code()), cella::db::ToString(perm.code()),
                          perm.message());
    }
    const DbStatus st = engine_->Checkpoint();
    if (!st.ok())
    {
      return FailResponse(500, cella::db::ToString(st.code()), st.message());
    }
    JsonValue data = JsonValue::Obj();
    data.Set("note", JsonValue::Str("数据文件已落盘（存盘点完成）"));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::Diagnostics(const HttpRequest &, const std::vector<std::string> &params)
  {
    const std::string kind = params.empty() ? std::string() : params[0];
    std::lock_guard<std::recursive_mutex> lk(gate_);
    // 缓冲池/锁表/事务表这类服务端诊断只给管理员
    const DbStatus perm = RequestSession().RequireAdmin("查看服务端诊断");
    if (!perm.ok())
    {
      return FailResponse(HttpStatusFor(perm.code()), cella::db::ToString(perm.code()),
                          perm.message());
    }
    std::string text;
    if (kind == "stats")
    {
      text = engine_->StatsText();
    }
    else if (kind == "locks")
    {
      text = engine_->LockText();
    }
    else if (kind == "waitfor")
    {
      text = engine_->WaitForGraphText();
    }
    else if (kind == "txn")
    {
      text = engine_->TxnText();
    }
    else
    {
      return HttpResponse::Error(404, "未知诊断项: " + kind);
    }
    JsonValue data = JsonValue::Obj();
    data.Set("kind", JsonValue::Str(kind));
    data.Set("title", JsonValue::Str(DiagTitle(kind)));
    data.Set("text", JsonValue::Str(text));
    return OkResponse(std::move(data));
  }

  // ── 目录 ────────────────────────────────────────────────────

  const CatalogTable *ApiService::FindTableLocked(const std::string &name)
  {
    return engine_->catalog().FindTable(name);
  }

  JsonValue ApiService::TableJsonLocked(const cella::db::CatalogTable &t)
  {
    JsonValue j = JsonValue::Obj();
    j.Set("name", JsonValue::Str(t.name));
    j.Set("tableId", JsonValue::Int(t.table_id));
    j.Set("createdAt", JsonValue::Int(t.created_at));
    JsonValue cols = JsonValue::Arr();
    JsonValue pk = JsonValue::Arr();
    std::int64_t ordinal = 0;
    for (const auto &c : t.columns)
    {
      ++ordinal;
      JsonValue cj = JsonValue::Obj();
      cj.Set("name", JsonValue::Str(c.name));
      cj.Set("type", JsonValue::Str(cella::db::CatalogTypeName(c.type)));
      cj.Set("typeFull", JsonValue::Str(std::string(cella::db::CatalogTypeName(c.type)) +
                                        (c.len > 0 ? "(" + std::to_string(c.len) + ")" : "")));
      cj.Set("len", JsonValue::Int(c.len));
      cj.Set("notNull", JsonValue::Bool(c.not_null));
      cj.Set("primaryKey", JsonValue::Bool(c.primary_key));
      cj.Set("ordinal", JsonValue::Int(ordinal));
      cols.Push(std::move(cj));
      if (c.primary_key)
      {
        pk.Push(JsonValue::Str(c.name)); // 数组形态：为表级复合主键预留（PLAN §6.1）
      }
    }
    j.Set("columns", std::move(cols));
    JsonValue pkobj = JsonValue::Obj();
    pkobj.Set("columns", std::move(pk));
    j.Set("primaryKey", std::move(pkobj));
    return j;
  }

  HttpResponse ApiService::Catalog(const HttpRequest &, const std::vector<std::string> &)
  {
    std::lock_guard<std::recursive_mutex> lk(gate_);
    // 元信息同样受访问控制：能访问本库才看得到表清单
    const DbStatus st = RequestSession().CheckDatabaseAccess(engine_->current_db());
    if (!st.ok())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    JsonValue arr = JsonValue::Arr();
    for (const CatalogTable *t : engine_->catalog().ListTables())
    {
      arr.Push(TableJsonLocked(*t));
    }
    JsonValue data = JsonValue::Obj();
    data.Set("currentDb", JsonValue::Str(engine_->current_db()));
    data.Set("tables", std::move(arr));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::CatalogOne(const HttpRequest &, const std::vector<std::string> &params)
  {
    const std::string name = params.empty() ? std::string() : params[0];
    std::lock_guard<std::recursive_mutex> lk(gate_);
    const DbStatus acc = RequestSession().CheckDatabaseAccess(engine_->current_db());
    if (!acc.ok())
    {
      return FailResponse(HttpStatusFor(acc.code()), cella::db::ToString(acc.code()), acc.message());
    }
    const CatalogTable *t = FindTableLocked(name);
    if (t == nullptr)
    {
      return FailResponse(404, "DB-502", "表不存在: " + name);
    }
    return OkResponse(TableJsonLocked(*t));
  }

  // ── 数据浏览 ────────────────────────────────────────────────

  JsonValue ApiService::ColumnsJsonLocked(const QueryResult &qr)
  {
    JsonValue cols = JsonValue::Arr();
    for (size_t i = 0; i < qr.columns.size(); ++i)
    {
      JsonValue cj = JsonValue::Obj();
      cj.Set("name", JsonValue::Str(qr.columns[i].name));
      cj.Set("type", JsonValue::Str(InferColumnType(qr, i)));
      cols.Push(std::move(cj));
    }
    return cols;
  }

  JsonValue ApiService::RowsJsonLocked(const QueryResult &qr, size_t max_rows, bool *truncated)
  {
    *truncated = qr.rows.size() > max_rows;
    JsonValue rows = JsonValue::Arr();
    const size_t n = std::min(qr.rows.size(), max_rows);
    for (size_t r = 0; r < n; ++r)
    {
      JsonValue row = JsonValue::Arr();
      for (const auto &v : qr.rows[r])
      {
        row.Push(ValueToJson(v));
      }
      rows.Push(std::move(row));
    }
    return rows;
  }

  HttpResponse ApiService::Rows(const HttpRequest &req, const std::vector<std::string> &params)
  {
    const std::string name = params.empty() ? std::string() : params[0];
    int page = 1;
    int page_size = kDefaultPageSize;
    std::string sort;
    std::string order = "asc";
    for (const auto &kv : req.QueryParams())
    {
      if (kv.first == "page")
      {
        page = std::atoi(kv.second.c_str());
      }
      else if (kv.first == "pageSize")
      {
        page_size = std::atoi(kv.second.c_str());
      }
      else if (kv.first == "sort")
      {
        sort = kv.second;
      }
      else if (kv.first == "order")
      {
        order = kv.second;
      }
    }
    page = std::max(1, page);
    page_size = std::clamp(page_size, 1, kMaxPageSize);
    if (order != "asc" && order != "desc")
    {
      order = "asc";
    }

    // 翻页也是一次真实查询（分页慢通常就是排序列没索引），耗时照实报给前端
    const auto t_req = std::chrono::steady_clock::now();

    std::lock_guard<std::recursive_mutex> lk(gate_);
    const CatalogTable *t = FindTableLocked(name);
    if (t == nullptr)
    {
      return FailResponse(404, "DB-502", "表不存在: " + name);
    }

    // 列清单：rowid 打头 + 全部真实列（`*` 是语句级独占标志，见 PLAN §6.3）
    std::vector<std::string> cols;
    cols.push_back("rowid");
    for (const auto &c : t->columns)
    {
      cols.push_back(c.name);
    }

    // 默认序（D9）：有主键 → 主键列；否则 rowid
    if (sort.empty())
    {
      sort = DefaultSortColumn(*t);
    }
    bool sort_ok = (sort == "rowid");
    for (const auto &c : t->columns)
    {
      sort_ok = sort_ok || (c.name == sort);
    }
    if (!sort_ok || !ValidIdentifier(sort))
    {
      return BodyError("排序列不存在: " + sort);
    }

    StatementOutcome out;
    const DbStatus st = RequestSession().ExecuteOne(
        SelectPageSql(t->name, cols, sort, order == "desc", page, page_size), 1, 1, &out);
    if (!st.ok() || !out.result.IsQuery())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }

    // 全量拉取模式（§11.7）：返回行数小于页大小时，说明已到表尾，总数已知
    const size_t got = out.result.rows.size();
    const bool total_known = got < static_cast<size_t>(page_size);

    bool truncated = false;
    JsonValue data = JsonValue::Obj();
    data.Set("table", JsonValue::Str(t->name));
    data.Set("page", JsonValue::Int(page));
    data.Set("pageSize", JsonValue::Int(page_size));
    data.Set("sort", JsonValue::Str(sort));
    data.Set("order", JsonValue::Str(order));
    data.Set("columns", ColumnsJsonLocked(out.result));
    data.Set("rows", RowsJsonLocked(out.result, kHardMaxRows, &truncated));
    data.Set("truncated", JsonValue::Bool(truncated));
    data.Set("hasPrimaryKey", JsonValue::Bool(PrimaryKeyOf(*t, nullptr)));
    data.Set("totalKnown", JsonValue::Bool(total_known));
    data.Set("total",
             total_known ? JsonValue::Int((static_cast<std::int64_t>(page) - 1) * page_size +
                                          static_cast<std::int64_t>(got))
                         : JsonValue::Null());
    data.Set("elapsedMs",
             JsonValue::Real(std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t_req)
                                 .count()));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::RowCount(const HttpRequest &, const std::vector<std::string> &params)
  {
    const std::string name = params.empty() ? std::string() : params[0];
    std::lock_guard<std::recursive_mutex> lk(gate_);
    const CatalogTable *t = FindTableLocked(name);
    if (t == nullptr)
    {
      return FailResponse(404, "DB-502", "表不存在: " + name);
    }
    if (t->columns.empty())
    {
      return OkResponse([]
                        {
      JsonValue d = JsonValue::Obj();
      d.Set("count", JsonValue::Int(0));
      return d; }());
    }
    // 方言没有聚合函数：取一列全量后计数（PLAN §6.2；小表已由全量拉取顺带解决）
    StatementOutcome out;
    const DbStatus st =
        RequestSession().ExecuteOne(CountSql(t->name, t->columns.front().name), 1, 1, &out);
    if (!st.ok() || !out.result.IsQuery())
    {
      return FailResponse(HttpStatusFor(st.code()), cella::db::ToString(st.code()), st.message());
    }
    JsonValue data = JsonValue::Obj();
    data.Set("count", JsonValue::Int(static_cast<std::int64_t>(out.result.rows.size())));
    return OkResponse(std::move(data));
  }

  // ── 行编辑（乐观校验）──────────────────────────────────────

  bool ApiService::KeyFromJsonLocked(const CatalogTable &table, const JsonValue &j, RowKey *key,
                                     std::string *err)
  {
    const JsonValue *kind = j.Find("kind");
    const JsonValue *cols = j.Find("columns");
    const JsonValue *vals = j.Find("values");
    if (kind == nullptr || !kind->IsString() || cols == nullptr || !cols->IsArray() ||
        vals == nullptr || !vals->IsArray() || cols->size() != vals->size() || cols->size() == 0)
    {
      *err = "定位键 key 应为 {kind, columns[], values[]} 且两数组等长";
      return false;
    }
    const std::string k = kind->AsString();
    if (k == "primary")
    {
      key->kind = RowKey::Kind::kPrimary;
    }
    else if (k == "rowid")
    {
      key->kind = RowKey::Kind::kRowid;
    }
    else if (k == "fullrow")
    {
      key->kind = RowKey::Kind::kFullRow;
    }
    else
    {
      *err = "未知定位键类型: " + k;
      return false;
    }
    for (size_t i = 0; i < cols->size(); ++i)
    {
      const std::string col = (*cols).items()[i].AsString();
      if (!ValidIdentifier(col))
      {
        *err = "定位键列名非法: " + col;
        return false;
      }
      const std::string decl = KeyColumnDeclType(table, col);
      if (decl.empty())
      {
        *err = "定位键列不存在: " + col;
        return false;
      }
      Value v;
      std::string why;
      if (!CellFromJson((*vals).items()[i], decl, &v, &why))
      {
        *err = "定位键值非法（列 " + col + "）：" + why;
        return false;
      }
      key->columns.push_back(col);
      key->values.push_back(std::move(v));
    }
    return true;
  }

  // 乐观校验公共流程：own_txn = 是否自管事务；write_sql 为 UPDATE/DELETE 文本
  HttpResponse ApiService::UpdateRow(const HttpRequest &req, const std::vector<std::string> &params)
  {
    return EditRow(req, params, /*is_delete=*/false);
  }

  HttpResponse ApiService::DeleteRow(const HttpRequest &req, const std::vector<std::string> &params)
  {
    return EditRow(req, params, /*is_delete=*/true);
  }

  HttpResponse ApiService::EditRow(const HttpRequest &req, const std::vector<std::string> &params,
                                   bool is_delete)
  {
    const std::string name = params.empty() ? std::string() : params[0];
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }

    std::lock_guard<std::recursive_mutex> lk(gate_);
    const CatalogTable *t = FindTableLocked(name);
    if (t == nullptr)
    {
      return FailResponse(404, "DB-502", "表不存在: " + name);
    }

    // 1) 定位键
    const JsonValue *kj = body.Find("key");
    RowKey key;
    std::string why;
    if (kj == nullptr || !KeyFromJsonLocked(*t, *kj, &key, &why))
    {
      return BodyError(why.empty() ? "缺少定位键 key" : why);
    }

    // 2) 待写入值（DELETE 无）
    std::vector<std::string> set_cols;
    std::vector<Value> set_vals;
    if (!is_delete)
    {
      const JsonValue *vals = body.Find("values");
      if (vals == nullptr || !vals->IsObject() || vals->members().empty())
      {
        return BodyError("缺少待写入的 values");
      }
      for (const auto &kv : vals->members())
      {
        const std::string col = kv.first;
        const CatalogColumn *meta = nullptr;
        for (const auto &c : t->columns)
        {
          if (c.name == col)
          {
            meta = &c;
            break;
          }
        }
        if (meta == nullptr)
        {
          return BodyError("列不存在: " + col);
        }
        Value v;
        if (!CellFromJson(kv.second, cella::db::CatalogTypeName(meta->type), &v, &why))
        {
          return BodyError("列 " + col + " 的值非法：" + why);
        }
        set_cols.push_back(col);
        set_vals.push_back(std::move(v));
      }
    }

    // 3) 期望旧值（乐观校验；可省略）
    std::vector<std::pair<std::string, Value>> expect;
    if (const JsonValue *ej = body.Find("expect"); ej != nullptr && ej->IsObject())
    {
      for (const auto &kv : ej->members())
      {
        const CatalogColumn *meta = nullptr;
        for (const auto &c : t->columns)
        {
          if (c.name == kv.first)
          {
            meta = &c;
            break;
          }
        }
        if (meta == nullptr)
        {
          return BodyError("expect 列不存在: " + kv.first);
        }
        Value v;
        if (!CellFromJson(kv.second, cella::db::CatalogTypeName(meta->type), &v, &why))
        {
          return BodyError("expect 列 " + kv.first + " 的值非法：" + why);
        }
        expect.emplace_back(kv.first, std::move(v));
      }
    }

    Session &session = RequestSession();
    const bool own_txn = !session.in_transaction();
    std::string note;
    if (own_txn)
    {
      const DbStatus b = session.Begin(&note);
      if (!b.ok())
      {
        return FailResponse(400, cella::db::ToString(b.code()), b.message());
      }
    }

    // 4) 重取该行（O(1) 直达当 key 为 rowid；主键/全列匹配走谓词过滤）
    std::vector<std::string> cols;
    for (const auto &c : t->columns)
    {
      cols.push_back(c.name);
    }
    const std::string fetch_sql = SelectByKeySql(t->name, cols, key);
    StatementOutcome fetch;
    const DbStatus fs = session.ExecuteOne(fetch_sql, 1, 1, &fetch);
    if (!fs.ok() || !fetch.result.IsQuery())
    {
      if (own_txn)
      {
        (void)session.Rollback(&note);
      }
      return FailResponse(400, cella::db::ToString(fs.code()), fs.message());
    }

    // 5) 比对期望旧值
    if (!expect.empty())
    {
      if (fetch.result.rows.empty())
      {
        if (own_txn)
        {
          (void)session.Rollback(&note);
        }
        return FailResponse(409, "ROW-GONE", kRowGone);
      }
      const std::vector<Value> &row = fetch.result.rows.front();
      for (const auto &[col, want] : expect)
      {
        for (size_t i = 0; i < fetch.result.columns.size(); ++i)
        {
          if (fetch.result.columns[i].name != col)
          {
            continue;
          }
          const Value have = i < row.size() ? row[i] : Value::Null();
          if (!cella::db::ValueEquals(want, have))
          {
            if (own_txn)
            {
              (void)session.Rollback(&note);
            }
            return FailResponse(409, "ROW-CHANGED", "该行在您编辑期间已被修改，已为您刷新");
          }
          break;
        }
      }
    }
    else if (fetch.result.rows.empty())
    {
      if (own_txn)
      {
        (void)session.Rollback(&note);
      }
      return FailResponse(409, "ROW-GONE", kRowGone);
    }

    // 6) 写入
    const std::string write_sql =
        is_delete ? DeleteSql(t->name, key) : UpdateSql(t->name, set_cols, set_vals, key);
    StatementOutcome write;
    const DbStatus ws = session.ExecuteOne(write_sql, 1, 1, &write);
    if (!ws.ok() || !write.status.ok() || !write.compile_errors.empty())
    {
      if (own_txn)
      {
        (void)session.Rollback(&note);
      }
      JsonValue e = StatementErrorJson(write);
      const std::string code =
          e.Find("code") != nullptr ? e.Find("code")->AsString() : cella::db::ToString(ws.code());
      const std::string msg = e.Find("message") != nullptr ? e.Find("message")->AsString()
                                                           : ws.message();
      return FailResponse(400, code, msg);
    }

    if (own_txn)
    {
      const DbStatus c = session.Commit(&note);
      if (!c.ok())
      {
        return FailResponse(400, cella::db::ToString(c.code()), c.message());
      }
    }

    JsonValue data = JsonValue::Obj();
    data.Set("affected", JsonValue::Int(static_cast<std::int64_t>(write.result.affected)));
    data.Set("tag", JsonValue::Str(write.result.tag));
    data.Set("elapsedMs", JsonValue::Real(write.elapsed_ms));
    data.Set("note", own_txn ? JsonValue::Str(note) : JsonValue::Str("复用当前事务（未提交）"));
    data.Set("inTxn", JsonValue::Bool(session.in_transaction()));
    return OkResponse(std::move(data));
  }

  HttpResponse ApiService::InsertRow(const HttpRequest &req, const std::vector<std::string> &params)
  {
    const std::string name = params.empty() ? std::string() : params[0];
    JsonValue body;
    HttpResponse err;
    if (!ParseBody(req, &body, &err))
    {
      return err;
    }
    std::lock_guard<std::recursive_mutex> lk(gate_);
    const CatalogTable *t = FindTableLocked(name);
    if (t == nullptr)
    {
      return FailResponse(404, "DB-502", "表不存在: " + name);
    }
    const JsonValue *vals = body.Find("values");
    if (vals == nullptr || !vals->IsObject())
    {
      return BodyError("缺少 values 对象");
    }

    // 显式全列清单：缺省列显式 NULL（方言不支持省略列的语义依赖，见 PLAN §4.3）
    std::vector<std::string> cols;
    std::vector<Value> values;
    std::string why;
    for (const auto &c : t->columns)
    {
      const JsonValue *v = vals->Find(c.name);
      Value cell;
      if (!CellFromJson(v != nullptr ? *v : JsonValue::Null(), cella::db::CatalogTypeName(c.type),
                        &cell, &why))
      {
        return BodyError("列 " + c.name + " 的值非法：" + why);
      }
      cols.push_back(c.name);
      values.push_back(std::move(cell));
    }

    StatementOutcome out;
    const DbStatus st =
        RequestSession().ExecuteOne(InsertSql(t->name, cols, values), 1, 1, &out);
    if (!st.ok() || !out.status.ok() || !out.compile_errors.empty())
    {
      JsonValue e = StatementErrorJson(out);
      const std::string code =
          e.Find("code") != nullptr ? e.Find("code")->AsString() : cella::db::ToString(st.code());
      const std::string msg =
          e.Find("message") != nullptr ? e.Find("message")->AsString() : st.message();
      return FailResponse(400, code, msg);
    }
    JsonValue data = JsonValue::Obj();
    data.Set("affected", JsonValue::Int(static_cast<std::int64_t>(out.result.affected)));
    data.Set("tag", JsonValue::Str(out.result.tag));
    data.Set("elapsedMs", JsonValue::Real(out.elapsed_ms));
    return OkResponse(std::move(data));
  }

} // namespace cella::client
