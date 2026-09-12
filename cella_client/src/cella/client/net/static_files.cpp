// static_files.cpp —— 静态资源实现：路径规范化 + MIME + 读文件。
#include "cella/client/net/static_files.h"

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace cella::client {

namespace {

namespace fs = std::filesystem;

const char* MimeType(const std::string& ext) {
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".js") return "text/javascript; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".woff2") return "font/woff2";
  return "application/octet-stream";
}

bool ReadWholeFile(const fs::path& path, std::string* out) {
  std::FILE* f = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(path.string().c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out->resize(static_cast<size_t>(size > 0 ? size : 0));
  const size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  return got == out->size();
}

}  // namespace

bool StaticFiles::Exists() const {
  std::error_code ec;
  return fs::is_directory(root_, ec);
}

bool StaticFiles::TryServe(const std::string& url_path, HttpResponse* resp) const {
  // 只接受以 '/' 开头的路径
  if (url_path.empty() || url_path[0] != '/') {
    *resp = HttpResponse::Error(400, "非法路径");
    return true;
  }
  std::string rel = url_path.substr(1);
  if (rel.empty()) {
    rel = "index.html";
  }

  // 路径穿越防护：拒绝 ".." 段、反斜杠、绝对形式与 NUL
  if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos ||
      rel.find('\0') != std::string::npos) {
    *resp = HttpResponse::Error(400, "路径穿越被拒绝");
    return true;
  }
  fs::path full = fs::path(root_) / fs::u8path(rel);
  std::error_code ec;
  if (!fs::is_regular_file(full, ec)) {
    *resp = HttpResponse::Error(404, "资源不存在: " + rel);
    return true;
  }
  // 双保险：规范化后必须仍位于 root 之内
  const fs::path canon_root = fs::weakly_canonical(fs::path(root_), ec);
  const fs::path canon_file = fs::weakly_canonical(full, ec);
  if (ec || canon_file.string().rfind(canon_root.string(), 0) != 0) {
    *resp = HttpResponse::Error(400, "路径穿越被拒绝");
    return true;
  }

  std::string body;
  if (!ReadWholeFile(canon_file, &body)) {
    *resp = HttpResponse::Error(500, "读取资源失败: " + rel);
    return true;
  }
  std::string ext = full.extension().string();
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  *resp = HttpResponse::Text(200, std::move(body), MimeType(ext));
  resp->headers.emplace_back("Cache-Control", "no-cache");
  return true;
}

}  // namespace cella::client
