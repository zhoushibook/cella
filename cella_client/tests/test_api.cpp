// test_api.cpp —— ApiService 端到端（不经过 socket）：
// 建表 / 插入 / 查询 / 目录 / 行编辑（乐观校验）/ 事务 / 坐标换算 / 错误映射。
//
// 注意响应外形：成功 { ok:true, data:{...} }；失败 { ok:false, error:{...} }。
// Fixture::Data 取 data 成员（缺失即失败并打印响应体），Fixture::Parse 取根。
#include <filesystem>
#include <iostream>
#include <string>

#include "cella/client/api/json.h"
#include "cella/client/server/api_service.h"
#include "mini_test.h"

namespace {

namespace fs = std::filesystem;

using cella::client::HttpRequest;
using cella::client::HttpResponse;
using cella::client::JsonDump;
using cella::client::JsonParse;
using cella::client::JsonValue;

struct Fixture {
  cella::db::DbEngine engine;
  cella::client::ApiService service;

  explicit Fixture(const std::string& name, bool auth = false)
      : engine(), service(&engine) {
    const std::string dir = std::string(CELLA_CLIENT_TESTDATA_DIR) + "/" + name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    cella::db::EngineConfig cfg;
    cfg.data_dir = dir;
    cfg.enable_log = false;
    cfg.enable_journal = false;
    cfg.enable_auth = auth;
    if (!engine.Open(cfg).ok()) {
      std::abort();
    }
  }
  ~Fixture() { engine.Close(); }

  static HttpRequest Make(const std::string& method, const std::string& target,
                          const std::string& body = std::string()) {
    HttpRequest req;
    req.method = method;
    req.target = target;
    // 与 HTTP 解析器一致：target 拆成 path + query
    const size_t q = target.find('?');
    if (q == std::string::npos) {
      req.path = target;
    } else {
      req.path = target.substr(0, q);
      req.query = target.substr(q + 1);
    }
    req.body = body;
    return req;
  }

  // 解析响应根对象
  static JsonValue Parse(const HttpResponse& r) {
    JsonValue v;
    if (!JsonParse(r.body, &v, nullptr)) {
      std::cerr << "[响应体解析失败] " << r.body << "\n" << std::flush;
      std::abort();
    }
    return v;
  }

  // 解析并取 data 成员（成功响应的业务数据）；缺失视为测试失败并打印响应体
  static JsonValue Data(const HttpResponse& r) {
    JsonValue v = Parse(r);
    const JsonValue* d = v.Find("data");
    if (d == nullptr) {
      std::cerr << "[响应缺少 data] " << r.body << "\n" << std::flush;
      std::abort();
    }
    return *d;
  }

  // 执行一段 SQL（走 /api/query），返回 data；失败打印并 abort
  JsonValue Run(const std::string& sql) {
    const auto resp = service.Handle(
        Make("POST", "/api/query", R"({"sql":")" + sql + R"("})"));
    return Data(resp);
  }

  // 带令牌的请求（Authorization: Bearer <token>）
  static HttpRequest WithToken(HttpRequest req, const std::string& token) {
    req.headers.push_back({"Authorization", "Bearer " + token});
    return req;
  }

  // 登录并返回令牌；失败返回空串
  std::string Login(const std::string& user, const std::string& password) {
    const std::string body = "{\"user\":\"" + user + "\",\"password\":\"" + password + "\"}";
    const HttpResponse resp = service.Handle(Make("POST", "/api/login", body));
    if (resp.status != 200) {
      return std::string();
    }
    const JsonValue v = Data(resp);
    const JsonValue* tok = v.Find("token");
    return (tok != nullptr && tok->IsString()) ? tok->AsString() : std::string();
  }

  // 失败响应的 error.code
  static std::string ErrorCode(const HttpResponse& r) {
    const auto v = Parse(r);
    const JsonValue* e = v.Find("error");
    return e != nullptr ? e->Find("code")->AsString() : std::string();
  }
};

}  // namespace

MT_TEST(API_健康与未知路由) {
  Fixture f("api_health");
  auto resp = f.service.Handle(Fixture::Make("GET", "/api/health"));
  MT_EQ(resp.status, 200);
  const auto v = Fixture::Parse(resp);
  MT_EQ(v.Find("ok")->AsBool(), true);
  MT_EQ(v.Find("data")->Find("opened")->AsBool(), true);

  auto nf = f.service.Handle(Fixture::Make("GET", "/api/不存在"));
  MT_EQ(nf.status, 404);
  auto bad = f.service.Handle(Fixture::Make("DELETE", "/api/health"));
  MT_EQ(bad.status, 405);
}

MT_TEST(API_建表插入查询闭环) {
  Fixture f("api_flow");
  (void)f.service.Handle(Fixture::Make(
      "POST", "/api/query",
      R"({"sql":"CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(16), score DOUBLE);"})"));

  const auto ins = f.Run("INSERT INTO student VALUES (1,'Alice',88.5),(2,'Bob',76.0),(3,'Carol',91.0);");
  MT_EQ(ins.Find("statements")->items()[0].Find("tag")->AsString(),
        std::string("INSERT 0 3"));

  const auto q = f.Run("get rowid, id, name, score in student ordered id asc;");
  const auto& st = q.Find("statements")->items()[0];
  MT_EQ(st.Find("ok")->AsBool(), true);
  MT_EQ(static_cast<int>(st.Find("columns")->size()), 4);
  MT_EQ(st.Find("columns")->items()[0].Find("name")->AsString(), std::string("rowid"));
  MT_EQ(st.Find("columns")->items()[0].Find("type")->AsString(), std::string("INT64"));
  MT_EQ(static_cast<int>(st.Find("rows")->size()), 3);
  MT_CHECK(st.Find("rows")->items()[0].items()[1].IsInt());
  MT_CHECK(st.Find("rows")->items()[2].items()[2].IsString());
  // 列类型推断：score 列全为数值
  MT_EQ(st.Find("columns")->items()[3].Find("type")->AsString(), std::string("DOUBLE"));
}

MT_TEST(API_目录带主键数组) {
  Fixture f("api_catalog");
  (void)f.Run("CREATE TABLE course(code VARCHAR(8) PRIMARY KEY NOT NULL, title TEXT);");
  auto resp = f.service.Handle(Fixture::Make("GET", "/api/catalog/course"));
  MT_EQ(resp.status, 200);
  const auto v = Fixture::Data(resp);
  MT_EQ(v.Find("name")->AsString(), std::string("course"));
  const auto pk = v.Find("primaryKey")->Find("columns");
  MT_EQ(static_cast<int>(pk->size()), 1);
  MT_EQ(pk->items()[0].AsString(), std::string("code"));
  const auto& c0 = v.Find("columns")->items()[0];
  MT_EQ(c0.Find("primaryKey")->AsBool(), true);
  MT_EQ(c0.Find("notNull")->AsBool(), true);
  MT_EQ(c0.Find("typeFull")->AsString(), std::string("VARCHAR(8)"));
}

MT_TEST(API_数据浏览行内含rowid) {
  Fixture f("api_rows");
  (void)f.Run("CREATE TABLE t(id INT, v VARCHAR(8));INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');");

  auto resp = f.service.Handle(Fixture::Make("GET", "/api/tables/t/rows?page=1&pageSize=2"));
  MT_EQ(resp.status, 200);
  const auto v = Fixture::Data(resp);
  MT_EQ(v.Find("page")->AsInt(), 1);
  MT_EQ(v.Find("pageSize")->AsInt(), 2);
  MT_EQ(v.Find("totalKnown")->AsBool(), false);  // 全量拉取未到表尾 → 总数未知
  MT_EQ(v.Find("hasPrimaryKey")->AsBool(), false);

  resp = f.service.Handle(Fixture::Make("GET", "/api/tables/t/rows?page=1&pageSize=200"));
  const auto full = Fixture::Data(resp);
  MT_EQ(full.Find("totalKnown")->AsBool(), true);  // §11.7：全量拉取总数自然已知
  MT_EQ(full.Find("total")->AsInt(), 3);

  resp = f.service.Handle(Fixture::Make("GET", "/api/tables/t/count"));
  MT_EQ(Fixture::Data(resp).Find("count")->AsInt(), 3);
}

MT_TEST(API_行编辑乐观校验) {
  Fixture f("api_edit");
  (void)f.Run("CREATE TABLE t(id INT, v VARCHAR(8));INSERT INTO t VALUES (1,'a'),(1,'a');");

  // 取两行（全列相同——正是 rowid 的用武之地）
  auto rows_resp = f.service.Handle(Fixture::Make("GET", "/api/tables/t/rows?page=1&pageSize=50"));
  const auto rows = Fixture::Data(rows_resp);
  const auto& r0 = rows.Find("rows")->items()[0];
  const auto& r1 = rows.Find("rows")->items()[1];
  const std::int64_t rowid0 = r0.items()[0].AsInt();
  const std::int64_t rowid1 = r1.items()[0].AsInt();
  MT_CHECK(rowid0 != rowid1);

  // 改第一行（rowid 定位 + expect 乐观校验）：另一行必须纹丝不动
  const std::string patch = R"({"key":{"kind":"rowid","columns":["rowid"],"values":[)" +
                            std::to_string(rowid0) +
                            R"(]},"expect":{"id":1,"v":"a"},"values":{"v":"changed"}})";
  auto resp = f.service.Handle(Fixture::Make("PATCH", "/api/tables/t/rows", patch));
  MT_EQ(resp.status, 200);

  rows_resp = f.service.Handle(Fixture::Make("GET", "/api/tables/t/rows?page=1&pageSize=50"));
  const auto after = Fixture::Data(rows_resp);
  // 行 = [rowid, id, v]（v 在第 3 列）。§11.3 实测：UPDATE = 删旧+插新，
  // 被改的行物理移到表尾（rowid 变化）——所以按 rowid 升序时它在第二行。
  MT_EQ(after.Find("rows")->items()[0].items()[2].AsString(), std::string("a"));
  MT_EQ(after.Find("rows")->items()[1].items()[2].AsString(), std::string("changed"));
  MT_CHECK(after.Find("rows")->items()[1].items()[0].AsInt() != rowid0);  // rowid 已变

  // 陈旧 rowid → 409 ROW-GONE（§11.4：陈旧与不存在都返回空结果）
  const std::string stale = R"({"key":{"kind":"rowid","columns":["rowid"],"values":[999999999]},)"
                            R"("values":{"v":"x"}})";
  resp = f.service.Handle(Fixture::Make("PATCH", "/api/tables/t/rows", stale));
  MT_EQ(resp.status, 409);
  MT_EQ(Fixture::ErrorCode(resp), std::string("ROW-GONE"));

  // 主键表：按主键定位改行（有主键优先于 rowid，PLAN §6.1 矩阵）
  (void)f.Run("CREATE TABLE p(id INT PRIMARY KEY, v VARCHAR(8));INSERT INTO p VALUES (7,'x');");
  const std::string pk_patch =
      R"({"key":{"kind":"primary","columns":["id"],"values":[7]},"expect":{"id":7,"v":"x"},"values":{"v":"y"}})";
  resp = f.service.Handle(Fixture::Make("PATCH", "/api/tables/p/rows", pk_patch));
  MT_EQ(resp.status, 200);

  // 主键冲突走 DB-516
  resp = f.service.Handle(Fixture::Make("POST", "/api/tables/p/rows",
                                        R"({"values":{"id":7,"v":"z"}})"));
  MT_EQ(resp.status, 400);
  MT_EQ(Fixture::ErrorCode(resp), std::string("DB-516"));

  // 删除行
  const std::string del = R"({"key":{"kind":"rowid","columns":["rowid"],"values":[)" +
                          std::to_string(rowid1) + R"(]},"expect":{"id":1,"v":"a"}})";
  resp = f.service.Handle(Fixture::Make("DELETE", "/api/tables/t/rows", del));
  MT_EQ(resp.status, 200);
}

MT_TEST(API_事务与诊断) {
  Fixture f("api_txn");
  auto resp = f.service.Handle(Fixture::Make("POST", "/api/txn/begin"));
  MT_EQ(resp.status, 200);
  MT_CHECK(Fixture::Data(resp).Find("inTxn")->AsBool());

  resp = f.service.Handle(Fixture::Make("POST", "/api/txn/begin"));  // 重复 BEGIN
  MT_EQ(resp.status, 400);
  MT_EQ(Fixture::ErrorCode(resp), std::string("DB-602"));

  resp = f.service.Handle(Fixture::Make("POST", "/api/txn/rollback"));
  MT_EQ(resp.status, 200);

  resp = f.service.Handle(Fixture::Make("GET", "/api/diagnostics/stats"));
  MT_EQ(resp.status, 200);
  MT_CHECK(!Fixture::Data(resp).Find("text")->AsString().empty());
}

MT_TEST(API_坐标换算黄金样本_单行) {
  // §11.6 修复后的黄金数据：单行语句诊断行 = 1 → 绝对行 = 语句起点行
  Fixture f("api_coord1");
  const std::string sql =
      "CREATE TABLE t(id INT);\n"     // 脚本行 1
      "get id in t;\n"                 // 行 2（正常）
      "get * in nope1;\n"              // 行 3（错误）
      "get * in nope2;\n";             // 行 4（错误）
  const auto v = f.Run(sql);
  const auto& stmts = v.Find("statements")->items();
  MT_EQ(static_cast<int>(stmts.size()), 4);
  const auto& s3 = stmts[2];
  MT_EQ(s3.Find("line")->AsInt(), 3);
  MT_CHECK(!s3.Find("ok")->AsBool());
  const auto e = s3.Find("error");
  MT_EQ(e->Find("code")->AsString(), std::string("SEM-301"));
  MT_EQ(e->Find("line")->AsInt(), 1);                    // 语句内行号（修复后无 +1 偏移）
  MT_EQ(e->Find("absLine")->AsInt(), 3);                 // 换算：3 + (1 - 1)
  MT_EQ(e->Find("col")->AsInt(), 10);
  const auto& s4 = stmts[3];
  MT_EQ(s4.Find("error")->Find("absLine")->AsInt(), 4);
}

MT_TEST(API_坐标换算黄金样本_多行) {
  // §11.6：跨 4–7 行的语句，错误在第 6 行 → 诊断行 3 → 绝对行 = 4 + (3 - 1) = 6
  Fixture f("api_coord2");
  const std::string sql =
      "CREATE TABLE t(id INT);\n"   // 行 1
      "get id in t;\n"               // 行 2
      "get id in t;\n"               // 行 3
      "get id,\n"                    // 行 4（语句开始）
      "  name\n"                     // 行 5
      "  in nope\n"                  // 行 6（错误）
      "  limit id = 1;\n";           // 行 7
  const auto v = f.Run(sql);
  const auto& s = v.Find("statements")->items()[3];
  MT_EQ(s.Find("line")->AsInt(), 4);
  const auto e = s.Find("error");
  MT_EQ(e->Find("line")->AsInt(), 3);
  MT_EQ(e->Find("absLine")->AsInt(), 6);
  MT_EQ(e->Find("col")->AsInt(), 6);
}

// ═════════════════════ 访问控制（服务端）════════════════════

MT_TEST(API_未登录被拒401) {
  Fixture f("api_auth_401", true);
  const auto denied = f.service.Handle(
      Fixture::Make("POST", "/api/query", R"({"sql":"get * in cella_catalog;"})"));
  MT_EQ(denied.status, 401);
  MT_EQ(Fixture::ErrorCode(denied), std::string("HTTP-401"));

  // 公开端点仍可访问（前端靠 authEnabled 判断是否需要弹登录框）
  const auto h = f.service.Handle(Fixture::Make("GET", "/api/health"));
  MT_EQ(h.status, 200);
  const auto hd = Fixture::Data(h);
  MT_EQ(hd.Find("authEnabled")->AsBool(), true);
  MT_EQ(hd.Find("user")->AsString(), std::string(""));

  // 静态资源不受访问控制影响（未设置 web 目录时为 404，但绝不能是 401）
  const auto page = f.service.Handle(Fixture::Make("GET", "/index.html"));
  MT_CHECK(page.status != 401);
}

MT_TEST(API_登录与令牌) {
  Fixture f("api_auth_login", true);
  // 错口令 → 401 + DB-801
  const auto bad = f.service.Handle(
      Fixture::Make("POST", "/api/login", R"({"user":"root","password":"nope"})"));
  MT_EQ(bad.status, 401);
  MT_EQ(Fixture::ErrorCode(bad), std::string("DB-801"));

  // root 初始空口令
  const std::string token = f.Login("root", "");
  MT_CHECK(!token.empty());

  const auto ok = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/query", R"({"sql":"CREATE TABLE t(id INT);"})"), token));
  MT_EQ(ok.status, 200);

  const auto sess =
      f.service.Handle(Fixture::WithToken(Fixture::Make("GET", "/api/session"), token));
  const auto sd = Fixture::Data(sess);
  MT_EQ(sd.Find("user")->AsString(), std::string("root"));
  MT_EQ(sd.Find("admin")->AsBool(), true);

  // 伪造令牌 → 401
  const auto fake = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/query", R"({"sql":"get * in cella_catalog;"})"), "deadbeef"));
  MT_EQ(fake.status, 401);

  // 登出后令牌立即失效
  const auto lo = f.service.Handle(Fixture::WithToken(Fixture::Make("POST", "/api/logout"), token));
  MT_EQ(lo.status, 200);
  const auto after = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/query", R"({"sql":"get * in cella_catalog;"})"), token));
  MT_EQ(after.status, 401);
}

MT_TEST(API_认证未启用时登录被拒) {
  Fixture f("api_auth_off");
  const auto login = f.service.Handle(
      Fixture::Make("POST", "/api/login", R"({"user":"root","password":""})"));
  MT_EQ(login.status, 400);
  // 未启用认证时一切照常免登录
  const auto q = f.service.Handle(
      Fixture::Make("POST", "/api/query", R"({"sql":"CREATE TABLE t(id INT);"})"));
  MT_EQ(q.status, 200);
  const auto h = f.service.Handle(Fixture::Make("GET", "/api/health"));
  MT_EQ(Fixture::Data(h).Find("authEnabled")->AsBool(), false);
}

MT_TEST(API_权限不足) {
  Fixture f("api_auth_perm", true);
  const std::string root = f.Login("root", "");
  MT_CHECK(!root.empty());
  auto run_as_root = [&](const std::string& sql) {
    return f.service.Handle(Fixture::WithToken(
        Fixture::Make("POST", "/api/query", "{\"sql\":\"" + sql + "\"}"), root));
  };
  MT_EQ(run_as_root("CREATE TABLE t(id INT);CREATE TABLE u(id INT);").status, 200);
  MT_EQ(run_as_root("CREATE USER alice IDENTIFIED BY 'p';").status, 200);
  MT_EQ(run_as_root("GRANT get ON main.t TO alice;").status, 200);

  const std::string alice = f.Login("alice", "p");
  MT_CHECK(!alice.empty());

  // 已授权的读放行
  const auto read = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/query", R"({"sql":"get * in t;"})"), alice));
  MT_EQ(read.status, 200);
  const JsonValue read_data = Fixture::Data(read);
  MT_EQ(read_data.Find("statements")->items()[0].Find("ok")->AsBool(), true);

  // 未授权的写：语句级错误码 DB-802（与编译错误一致，前端按语句提示）
  const auto write = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/query", R"({"sql":"INSERT INTO t VALUES (1);"})"), alice));
  MT_EQ(write.status, 200);
  const JsonValue write_data = Fixture::Data(write);
  const JsonValue& write_st = write_data.Find("statements")->items()[0];
  MT_EQ(write_st.Find("ok")->AsBool(), false);
  MT_EQ(write_st.Find("error")->Find("code")->AsString(), std::string("DB-802"));

  // 数据浏览端点走同一套判定：有读权限的表 200、无权限的表 403
  const auto rows_t = f.service.Handle(Fixture::WithToken(
      Fixture::Make("GET", "/api/tables/t/rows?page=1&pageSize=10"), alice));
  MT_EQ(rows_t.status, 200);
  const auto rows_u = f.service.Handle(Fixture::WithToken(
      Fixture::Make("GET", "/api/tables/u/rows?page=1&pageSize=10"), alice));
  MT_EQ(rows_u.status, 403);
  MT_EQ(Fixture::ErrorCode(rows_u), std::string("DB-802"));

  // 建库需要管理员（不能靠直连引擎端点绕过）
  const auto madedb = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/databases/create", R"({"name":"school"})"), alice));
  MT_EQ(madedb.status, 403);
  MT_EQ(Fixture::ErrorCode(madedb), std::string("DB-802"));
  const auto madedb_ok = f.service.Handle(Fixture::WithToken(
      Fixture::Make("POST", "/api/databases/create", R"({"name":"school"})"), root));
  MT_EQ(madedb_ok.status, 200);

  // 服务端诊断只给管理员
  const auto diag = f.service.Handle(
      Fixture::WithToken(Fixture::Make("GET", "/api/diagnostics/stats"), alice));
  MT_EQ(diag.status, 403);
  MT_EQ(f.service.Handle(Fixture::WithToken(Fixture::Make("GET", "/api/diagnostics/stats"), root))
            .status,
        200);

  // 库列表按权限过滤：alice 在 main 上有授权 → 看得到 main，看不到新建的 school
  const auto dbs = f.service.Handle(Fixture::WithToken(Fixture::Make("GET", "/api/databases"), alice));
  MT_EQ(dbs.status, 200);
  const JsonValue dbs_data = Fixture::Data(dbs);
  MT_EQ(static_cast<int>(dbs_data.Find("databases")->size()), 1);
  MT_EQ(dbs_data.Find("databases")->items()[0].Find("name")->AsString(), std::string("main"));
}
