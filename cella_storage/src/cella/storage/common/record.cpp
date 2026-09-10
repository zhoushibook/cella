#include "cella/storage/common/record.h"

#include <sstream>

namespace cella::storage {

std::string Record::ToString() const {
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << values_[i].ToString();
  }
  os << "]";
  return os.str();
}

}  // namespace cella::storage
