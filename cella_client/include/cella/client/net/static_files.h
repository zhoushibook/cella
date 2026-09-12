// static_files.h —— web/ 目录的静态资源服务（含路径穿越防护）。
#pragma once

#include <string>

#include "cella/client/net/http_types.h"

namespace cella::client {

class StaticFiles {
 public:
  explicit StaticFiles(std::string root) : root_(std::move(root)) {}

  // 尝试按路径返回文件。命中返回 true 并填充 resp；
  // 未命中（404）或路径非法（穿越）返回 false，resp 填好对应错误响应。
  bool TryServe(const std::string& url_path, HttpResponse* resp) const;

  const std::string& root() const { return root_; }
  bool Exists() const;

 private:
  std::string root_;  // 绝对路径
};

}  // namespace cella::client
