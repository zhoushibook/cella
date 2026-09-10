// time_util.h —— 时间工具（日志/目录时间戳统一格式）。
#pragma once

#include <cstdint>
#include <string>

namespace cella::db {

// "YYYY-MM-DD HH:MM:SS" 本地时间
std::string NowDateTime();
// "YYYY-MM-DDTHH:MM:SS" ISO 风格（日志文件用）
std::string NowIso();
// Unix 秒
int64_t NowEpochSeconds();

// 毫秒级耗时格式化（性能统计打印用）
std::string FormatMillis(double ms);

}  // namespace cella::db
