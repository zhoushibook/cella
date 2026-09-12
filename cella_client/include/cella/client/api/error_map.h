// error_map.h —— 把引擎的两种错误折叠成统一 JSON error 对象。
//
// 统一响应外形（PLAN §4.3）：
//   成功  { "ok": true,  "data": {...} }
//   失败  { "ok": false, "error": { code, message, line, col, detail } }
#pragma once

#include "cella/cella_common.h"
#include "cella/client/api/json.h"
#include "cella/db/common/db_status.h"
#include "cella/db/engine/db_engine.h"

namespace cella::client {

// 编译器诊断 → {code:"SEM-301", message, line, col}
cella::client::JsonValue CompileErrorJson(const cella::CELLA_Error& e);

// DbStatus → {code:"DB-502", message}
JsonValue DbStatusJson(const cella::db::DbStatus& s);

// 语句级错误：优先编译器诊断（更精确），否则 DbStatus；两者皆无 → null
// detail 字段带上全部诊断原文（前端「查看详情」用）
JsonValue StatementErrorJson(const cella::db::StatementOutcome& s);

// 失败响应体：{ok:false, error:{...}}
JsonValue ErrorBody(const std::string& code, const std::string& message, int line = 0, int col = 0,
                    const std::string& detail = std::string());

}  // namespace cella::client
