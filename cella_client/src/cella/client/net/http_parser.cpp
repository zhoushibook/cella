// http_parser.cpp —— HTTP 请求解析实现。
#include "cella/client/net/http_parser.h"

#include <cctype>
#include <cstdlib>

#include "cella/client/api/json.h"

namespace cella::client {

std::string HttpRequest::Header(const std::string& name) const {
  auto lower = [](const std::string& s) {
    std::string out;
    for (const char c : s) {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
  };
  const std::string want = lower(name);
  for (const auto& kv : headers) {
    if (lower(kv.first) == want) {
      return kv.second;
    }
  }
  return std::string();
}

std::vector<std::pair<std::string, std::string>> HttpRequest::QueryParams() const {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < query.size()) {
    size_t amp = query.find('&', i);
    if (amp == std::string::npos) {
      amp = query.size();
    }
    const std::string pair = query.substr(i, amp - i);
    i = amp + 1;
    if (pair.empty()) {
      continue;
    }
    const size_t eq = pair.find('=');
    if (eq == std::string::npos) {
      out.emplace_back(UrlDecode(pair), std::string());
    } else {
      out.emplace_back(UrlDecode(pair.substr(0, eq)), UrlDecode(pair.substr(eq + 1)));
    }
  }
  return out;
}

std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

HttpResponse HttpResponse::Json(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

HttpResponse HttpResponse::Text(int status, std::string body, const char* content_type) {
  HttpResponse r;
  r.status = status;
  r.content_type = content_type;
  r.body = std::move(body);
  return r;
}

HttpResponse HttpResponse::Error(int status, const std::string& message) {
  JsonValue err = JsonValue::Obj();
  err.Set("code", JsonValue::Str("HTTP-" + std::to_string(status)));
  err.Set("message", JsonValue::Str(message));
  JsonValue root = JsonValue::Obj();
  root.Set("ok", JsonValue::Bool(false));
  root.Set("error", std::move(err));
  return Json(status, JsonDump(root));
}

const char* StatusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "OK";
  }
}

// ── 解析器 ──────────────────────────────────────────────────

void HttpParser::Feed(const char* data, size_t len) {
  if (finished_) {
    return;
  }
  buf_.append(data, len);

  if (!header_done_) {
    const size_t head_end = buf_.find("\r\n\r\n");
    if (head_end == std::string::npos) {
      if (buf_.size() > kMaxHeaderBytes) {
        result_.status = ParseStatus::kError;
        result_.error_status = 413;
        result_.error_text = "请求头过大";
        finished_ = true;
      }
      return;
    }

    // 请求行
    const std::string head = buf_.substr(0, head_end);
    size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);
    {
      size_t sp1 = request_line.find(' ');
      size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                              : request_line.find(' ', sp1 + 1);
      if (sp1 == std::string::npos || sp2 == std::string::npos) {
        result_.status = ParseStatus::kError;
        result_.error_status = 400;
        result_.error_text = "畸形请求行";
        finished_ = true;
        return;
      }
      result_.req.method = request_line.substr(0, sp1);
      result_.req.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
      const std::string version = request_line.substr(sp2 + 1);
      if (version.rfind("HTTP/1.", 0) != 0) {
        result_.status = ParseStatus::kError;
        result_.error_status = 501;
        result_.error_text = "仅支持 HTTP/1.x";
        finished_ = true;
        return;
      }
    }
    const size_t qmark = result_.req.target.find('?');
    if (qmark == std::string::npos) {
      result_.req.path = UrlDecode(result_.req.target);
    } else {
      result_.req.path = UrlDecode(result_.req.target.substr(0, qmark));
      result_.req.query = result_.req.target.substr(qmark + 1);
    }

    // 头部
    size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
    while (pos < head.size()) {
      size_t nl = head.find("\r\n", pos);
      if (nl == std::string::npos) {
        nl = head.size();
      }
      const std::string line = head.substr(pos, nl - pos);
      pos = nl + 2;
      if (line.empty()) {
        continue;
      }
      const size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;  // 容忍无冒号的坏行
      }
      size_t vstart = colon + 1;
      while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t')) {
        ++vstart;
      }
      size_t vend = line.size();
      while (vend > vstart && (line[vend - 1] == ' ' || line[vend - 1] == '\t')) {
        --vend;
      }
      result_.req.headers.emplace_back(line.substr(0, colon), line.substr(vstart, vend - vstart));
    }

    header_done_ = true;
    result_.consumed = head_end + 4;

    // 正文长度
    const std::string te = result_.req.Header("Transfer-Encoding");
    if (!te.empty() && te.find("chunked") != std::string::npos) {
      result_.status = ParseStatus::kError;
      result_.error_status = 501;
      result_.error_text = "不支持 chunked 请求体";
      finished_ = true;
      return;
    }
    const std::string cl = result_.req.Header("Content-Length");
    if (!cl.empty()) {
      char* end = nullptr;
      const unsigned long long v = std::strtoull(cl.c_str(), &end, 10);
      if (end == nullptr || *end != '\0') {
        result_.status = ParseStatus::kError;
        result_.error_status = 400;
        result_.error_text = "Content-Length 非法";
        finished_ = true;
        return;
      }
      if (v > kMaxBodyBytes) {
        result_.status = ParseStatus::kError;
        result_.error_status = 413;
        result_.error_text = "请求体过大";
        finished_ = true;
        return;
      }
      body_needed_ = static_cast<size_t>(v);
    }
  }

  // 正文
  const size_t have = buf_.size() - result_.consumed;
  if (have < body_needed_) {
    if (buf_.size() > kMaxHeaderBytes + kMaxBodyBytes) {
      result_.status = ParseStatus::kError;
      result_.error_status = 413;
      result_.error_text = "请求体过大";
      finished_ = true;
    }
    return;
  }
  result_.req.body = buf_.substr(result_.consumed, body_needed_);
  result_.consumed += body_needed_;
  result_.status = ParseStatus::kComplete;
  finished_ = true;
}

}  // namespace cella::client
