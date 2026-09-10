#include "cella/storage/common/status.h"

#include <utility>

namespace cella::storage {

const char* ToString(StatusCode c) {
  switch (c) {
    case StatusCode::kOk:                 return "kOk";
    case StatusCode::kIoError:            return "kIoError";
    case StatusCode::kInvalidConfig:      return "kInvalidConfig";
    case StatusCode::kPageNotFound:       return "kPageNotFound";
    case StatusCode::kNoFreeFrame:        return "kNoFreeFrame";
    case StatusCode::kNoFreePage:         return "kNoFreePage";
    case StatusCode::kPageFull:           return "kPageFull";
    case StatusCode::kRecordTooLarge:     return "kRecordTooLarge";
    case StatusCode::kInvalidArgument:    return "kInvalidArgument";
    case StatusCode::kCorruptPage:        return "kCorruptPage";
    case StatusCode::kVersionMismatch:    return "kVersionMismatch";
    case StatusCode::kTableNotFound:      return "kTableNotFound";
    case StatusCode::kTableAlreadyExists: return "kTableAlreadyExists";
    case StatusCode::kTypeMismatch:       return "kTypeMismatch";
    case StatusCode::kNotImplemented:     return "kNotImplemented";
  }
  return "kUnknown";
}

Status::Status(StatusCode code, std::string msg)
    : code_(code), msg_(std::move(msg)) {}

std::string Status::ToString() const {
  if (msg_.empty()) {
    return cella::storage::ToString(code_);
  }
  return std::string(cella::storage::ToString(code_)) + ": " + msg_;
}

}  // namespace cella::storage
