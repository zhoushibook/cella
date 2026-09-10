#include "cella/storage/common/schema.h"

namespace cella::storage {

void Schema::AddColumn(const std::string& name, ValueType type, uint16_t max_len) {
  columns_.push_back(Column{name, type, max_len});
}

}  // namespace cella::storage
