#include "cella/storage/common/buffer_stats.h"

#include <iomanip>
#include <sstream>

namespace cella::storage {

std::string BufferStats::ToLogLine() const {
  std::ostringstream os;
  os << "accesses=" << access << " hits=" << hit << " misses=" << miss
     << " hit_rate=" << std::fixed << std::setprecision(2) << (hit_rate() * 100.0) << "%"
     << " evictions=" << evict;
  return os.str();
}

std::string BufferStats::ToString() const {
  std::ostringstream os;
  os << std::left
     << "  " << std::setw(13) << "accesses"    << ": " << access << "\n"
     << "  " << std::setw(13) << "hits"        << ": " << hit << "\n"
     << "  " << std::setw(13) << "misses"      << ": " << miss << "\n"
     << "  " << std::setw(13) << "hit_rate"    << ": " << std::fixed << std::setprecision(2)
     << (hit_rate() * 100.0) << "%\n"
     << "  " << std::setw(13) << "evictions"   << ": " << evict << "\n"
     << "  " << std::setw(13) << "dirty_flush" << ": " << dirty_flush << "\n"
     << "  " << std::setw(13) << "disk_reads"  << ": " << disk_reads << "\n"
     << "  " << std::setw(13) << "disk_writes" << ": " << disk_writes << "\n"
     << "  " << std::setw(13) << "page_allocs" << ": " << page_allocs << "\n"
     << "  " << std::setw(13) << "page_frees"  << ": " << page_frees;
  return os.str();
}

}  // namespace cella::storage
