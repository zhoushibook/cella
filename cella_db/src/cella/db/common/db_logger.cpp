#include "cella/db/common/db_logger.h"

namespace cella::db {

DbLogger& DbLogger::Global() {
  static DbLogger instance;
  return instance;
}

}  // namespace cella::db
