// error_map.cpp —— 错误折叠实现。
//
// 注意（PLAN §11.6）：编译器诊断的 line/col 是「语句内坐标」，
// 换算绝对行 = StatementOutcome.line + (诊断行 - 1)。这里输出的 line/col
// 保留诊断原值，并由本函数统一完成换算 —— 前端拿到即可直接定位。
#include "cella/client/api/error_map.h"

namespace cella::client {

using cella::db::StatementOutcome;

JsonValue CompileErrorJson(const cella::CELLA_Error& e) {
  JsonValue v = JsonValue::Obj();
  v.Set("code", JsonValue::Str(e.code));
  v.Set("message", JsonValue::Str(e.message));
  v.Set("line", JsonValue::Int(e.line));
  v.Set("col", JsonValue::Int(e.col));
  return v;
}

JsonValue DbStatusJson(const cella::db::DbStatus& s) {
  JsonValue v = JsonValue::Obj();
  v.Set("code", JsonValue::Str(cella::db::ToString(s.code())));
  v.Set("message", JsonValue::Str(s.message()));
  v.Set("line", JsonValue::Int(0));
  v.Set("col", JsonValue::Int(0));
  return v;
}

JsonValue StatementErrorJson(const StatementOutcome& s) {
  const bool has_compile = !s.compile_errors.empty();
  const bool has_status = !s.status.ok();
  if (!has_compile && !has_status) {
    return JsonValue::Null();
  }

  JsonValue err = has_compile ? CompileErrorJson(s.compile_errors.front())
                              : DbStatusJson(s.status);

  // detail：全部诊断原文（换算成脚本绝对行号后拼接）
  std::string detail;
  if (has_compile) {
    for (const auto& e : s.compile_errors) {
      if (!detail.empty()) {
        detail += "\n";
      }
      // 绝对行 = 语句起点行 + (诊断行 - 1)；列号不换算
      const int abs_line = s.line + e.line - 1;
      detail += std::string("[") + cella::cella_phaseText(e.phase) + "] " + e.code + " @" +
                std::to_string(abs_line) + ":" + std::to_string(e.col) + " " + e.message;
    }
  } else {
    detail = s.status.ToString();
  }
  err.Set("detail", JsonValue::Str(detail));

  // 换算后的绝对位置（前端直接用）
  if (has_compile) {
    err.Set("absLine", JsonValue::Int(s.line + err.Find("line")->AsInt() - 1));
  } else {
    err.Set("absLine", JsonValue::Int(s.line));
  }
  return err;
}

JsonValue ErrorBody(const std::string& code, const std::string& message, int line, int col,
                    const std::string& detail) {
  JsonValue err = JsonValue::Obj();
  err.Set("code", JsonValue::Str(code));
  err.Set("message", JsonValue::Str(message));
  err.Set("line", JsonValue::Int(line));
  err.Set("col", JsonValue::Int(col));
  if (!detail.empty()) {
    err.Set("detail", JsonValue::Str(detail));
  }
  JsonValue root = JsonValue::Obj();
  root.Set("ok", JsonValue::Bool(false));
  root.Set("error", std::move(err));
  return root;
}

}  // namespace cella::client
