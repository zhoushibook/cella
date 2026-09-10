#include "cella/db/exec/row_set.h"

#include "cella/cella_common.h"

namespace cella::db {

int RowSet::Resolve(const std::string& qualifier, const std::string& name) const {
  const std::string want_name = cella::cella_toUpper(name);
  const std::string want_qual = cella::cella_toUpper(qualifier);
  int found = -1;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (cella::cella_toUpper(fields[i].name) != want_name) {
      continue;
    }
    if (!want_qual.empty() && cella::cella_toUpper(fields[i].qualifier) != want_qual) {
      continue;
    }
    if (found >= 0) {
      return found;  // 重名：语义阶段应已拦下，这里取首个匹配
    }
    found = static_cast<int>(i);
  }
  return found;
}

}  // namespace cella::db
