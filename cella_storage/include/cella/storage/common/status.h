#pragma once
#include <cstdint>
#include <string>

namespace cella::storage {

// ── 错误码：一处定义，文档《API.md》据此生成对照表 ────────────
enum class StatusCode : uint8_t {
  kOk = 0,
  kIoError,
  kInvalidConfig,
  kPageNotFound,
  kNoFreeFrame,
  kNoFreePage,
  kPageFull,
  kRecordTooLarge,
  kInvalidArgument,
  kCorruptPage,
  kVersionMismatch,
  kTableNotFound,
  kTableAlreadyExists,
  kTypeMismatch,
  kNotImplemented,
};

const char* ToString(StatusCode c);

// ── 统一返回状态：每次 I/O 都必须检查返回值（约束 #9）────────
// [[nodiscard]] 使「忽略返回值」直接触发编译警告（-Werror 下即报错）。
class [[nodiscard]] Status {
 public:
  Status() = default;   // 默认成功
  Status(StatusCode code, std::string msg = "");

  static Status OK() { return Status(); }
  static Status Error(StatusCode c, std::string msg = "") {
    return Status(c, std::move(msg));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return msg_; }
  std::string ToString() const;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string msg_;
};

}  // namespace cella::storage
