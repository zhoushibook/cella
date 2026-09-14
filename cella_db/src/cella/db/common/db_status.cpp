#include "cella/db/common/db_status.h"

namespace cella::db {

const char* ToString(DbCode c) {
  switch (c) {
    case DbCode::kOk:                 return "DB-000";
    case DbCode::kSqlError:           return "DB-501";
    case DbCode::kTableNotFound:      return "DB-502";
    case DbCode::kTableExists:        return "DB-503";
    case DbCode::kColumnNotFound:     return "DB-504";
    case DbCode::kTypeMismatch:       return "DB-505";
    case DbCode::kNotNullViolation:   return "DB-506";
    case DbCode::kValueCountMismatch: return "DB-507";
    case DbCode::kValueTooLong:       return "DB-508";
    case DbCode::kUnknownColumn:      return "DB-509";
    case DbCode::kRecordTooLarge:     return "DB-510";
    case DbCode::kDivisionByZero:     return "DB-511";
    case DbCode::kSystemTableProtected: return "DB-512";
    case DbCode::kDatabaseTxnActive:  return "DB-513";
    case DbCode::kDatabaseError:      return "DB-514";
    case DbCode::kDatabaseProtected:  return "DB-515";
    case DbCode::kPrimaryKeyViolation: return "DB-516";
    case DbCode::kIndexExists:         return "DB-517";
    case DbCode::kIndexNotFound:       return "DB-518";
    case DbCode::kUniqueViolation:     return "DB-519";
    case DbCode::kStorageError:       return "DB-520";
    case DbCode::kWalError:           return "DB-521";
    case DbCode::kNoActiveTxn:        return "DB-601";
    case DbCode::kTxnAlreadyActive:   return "DB-602";
    case DbCode::kTxnAborted:         return "DB-603";
    case DbCode::kDeadlock:           return "DB-604";
    case DbCode::kLockConflict:       return "DB-605";
    case DbCode::kCatalogError:       return "DB-701";
    case DbCode::kSessionError:       return "DB-702";
    case DbCode::kNotImplemented:     return "DB-703";
    case DbCode::kInternal:           return "DB-704";
    case DbCode::kAuthFailed:         return "DB-801";
    case DbCode::kPermissionDenied:   return "DB-802";
    case DbCode::kUserError:          return "DB-803";
    case DbCode::kLastAdmin:          return "DB-804";
    case DbCode::kGrantDenied:        return "DB-805";
    case DbCode::kNoCredentials:      return "DB-806";
  }
  return "DB-???";
}

DbStatus::DbStatus(DbCode code, std::string message)
    : code_(code), msg_(std::move(message)) {}

std::string DbStatus::ToString() const {
  if (ok()) {
    return "[DB-000] OK";
  }
  // 限定调用：类内同名成员 ToString() 会遮蔽同命名空间的自由函数
  return std::string("[") + ::cella::db::ToString(code_) + "] " + msg_;
}

}  // namespace cella::db
