// http_parser.h —— 请求行 + 头部 + 定长正文的增量解析。
//
// 只实现 SPA 客户端会用到的子集：GET/POST、Content-Length 定长正文。
// 不支持 chunked（前端不会发）；头部/正文有大小上限，防恶意请求。
#pragma once

#include <string>

#include "cella/client/net/http_types.h"

namespace cella::client {

enum class ParseStatus {
  kNeedMore,   // 数据不足，继续 recv
  kComplete,   // 解析成功，req 已填充
  kError,      // 畸形请求（应回 400/413/501）
};

struct ParseResult {
  ParseStatus status = ParseStatus::kNeedMore;
  size_t consumed = 0;  // kComplete 时：本请求消耗的字节数（支持 keep-alive 粘包）
  HttpRequest req;
  int error_status = 400;  // kError 时的建议状态码
  std::string error_text;
};

class HttpParser {
 public:
  // 喂入一段数据（可多次调用，内部累积）。达到 kComplete/kError 后不再接收。
  void Feed(const char* data, size_t len);
  ParseResult result() const { return result_; }

  static constexpr size_t kMaxHeaderBytes = 16 * 1024;
  static constexpr size_t kMaxBodyBytes = 8 * 1024 * 1024;

 private:
  ParseResult result_;
  std::string buf_;
  bool header_done_ = false;
  size_t body_needed_ = 0;
  bool finished_ = false;
};

}  // namespace cella::client
