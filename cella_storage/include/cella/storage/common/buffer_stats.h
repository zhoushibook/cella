#pragma once
#include <cstdint>
#include <string>

namespace cella::storage {

// ── 缓冲池统计（§10）────────────────────────────────────────
struct BufferStats {
  uint64_t access = 0;
  uint64_t hit = 0;
  uint64_t miss = 0;
  uint64_t evict = 0;
  uint64_t dirty_flush = 0;
  uint64_t disk_reads = 0;
  uint64_t disk_writes = 0;
  uint64_t page_allocs = 0;
  uint64_t page_frees = 0;

  double hit_rate() const {
    return access == 0 ? 0.0 : static_cast<double>(hit) / static_cast<double>(access);
  }

  std::string ToLogLine() const;   // 单行：accesses=.. hits=.. hit_rate=..%
  std::string ToString() const;    // 表格化（多行）
};

}  // namespace cella::storage
