#include "cella/storage/record/slotted_record_serializer.h"

#include <cstring>
#include <vector>

#include "cella/storage/common/byte_buffer.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 记录二进制格式（一条记录 = 一行数据，被序列化成一段字节流后塞进页里）。
//
//   | col_count:u16 | null_bitmap:ceil(n/8) | 列数据 |
//
//   - col_count：这一行有几列（= schema 的列数）。
//   - null_bitmap：位图，第 i 位 = 1 表示第 i 列是 NULL。ceil(n/8) 字节。
//   - 列数据：按 schema 顺序，跳过 NULL 列，逐个写非空列的值。
//     定长类型按小端写；VARCHAR 是「u16 长度 + 字节」。
//
// 反序列化需要 schema（才能知道每列的类型、宽度），所以 schema 会被持久化
// 到目录里（见 file_storage.cpp）。
// ─────────────────────────────────────────────────────────────────────────

Status SlottedRecordSerializer::Serialize(const Record& record, const Schema& schema,
                                          std::vector<char>* out) {
  const size_t n = schema.column_count();
  if (record.value_count() != n) {
    return Status::Error(StatusCode::kTypeMismatch, "记录列数与 schema 不一致");
  }

  ByteBuffer bb;

  // ① 列数
  bb.PutUint16(static_cast<uint16_t>(n));

  // ② null_bitmap：先扫一遍，把 NULL 列的对应位置 1
  const size_t bitmap_bytes = (n + 7) / 8;      // ceil(n/8)
  std::vector<char> bitmap(bitmap_bytes, 0);
  for (size_t i = 0; i < n; ++i) {
    if (record.value(i).IsNull()) {
      bitmap[i / 8] = static_cast<char>(bitmap[i / 8] | (1 << (i % 8)));
    }
  }
  bb.PutBytes(bitmap.data(), bitmap_bytes);

  // ③ 列数据：按 schema 类型，逐个写非 NULL 列
  for (size_t i = 0; i < n; ++i) {
    const Value& v = record.value(i);
    if (v.IsNull()) {
      continue;                              // NULL 列不占数据空间（bitmap 已记录）
    }
    const ValueType t = schema.column(i).type;
    switch (t) {
      case ValueType::kBool:
        bb.PutUint8(v.bool_val ? 1 : 0);
        break;
      case ValueType::kInt32:
        bb.PutUint32(static_cast<uint32_t>(v.int32_val));   // 小端 4 字节
        break;
      case ValueType::kInt64:
        bb.PutUint64(static_cast<uint64_t>(v.int64_val));
        break;
      case ValueType::kFloat: {
        // 浮点不能直接移位，把它按位转成 uint32 再小端写
        uint32_t bits = 0;
        std::memcpy(&bits, &v.float_val, sizeof(bits));
        bb.PutUint32(bits);
        break;
      }
      case ValueType::kDouble: {
        uint64_t bits = 0;
        std::memcpy(&bits, &v.double_val, sizeof(bits));
        bb.PutUint64(bits);
        break;
      }
      case ValueType::kVarchar:
      case ValueType::kChar:
        bb.PutString(v.str_val);             // 写「u16 长度 + 字节」
        break;
      default:
        return Status::Error(StatusCode::kTypeMismatch, "不支持的类型");
    }
  }

  *out = std::move(bb.data());
  return Status::OK();
}

Status SlottedRecordSerializer::Deserialize(const char* data, size_t len, const Schema& schema,
                                            Record* out) {
  ByteBuffer bb(std::vector<char>(data, data + len));

  // ① 列数（校验与 schema 一致）
  const uint16_t n = bb.GetUint16();
  if (n != schema.column_count()) {
    return Status::Error(StatusCode::kTypeMismatch, "记录列数与 schema 不一致");
  }

  // ② null_bitmap
  const size_t bitmap_bytes = (n + 7) / 8;
  const std::vector<char> bitmap = bb.GetBytes(bitmap_bytes);

  // ③ 逐列读回：先查 bitmap 判断是否 NULL，非 NULL 再按 schema 类型读
  Record rec;
  for (size_t i = 0; i < n; ++i) {
    const bool is_null = (bitmap[i / 8] & (1 << (i % 8))) != 0;
    if (is_null) {
      rec.AddValue(Value::Null());
      continue;
    }
    const ValueType t = schema.column(i).type;
    switch (t) {
      case ValueType::kBool:
        rec.AddValue(Value::Bool(bb.GetUint8() != 0));
        break;
      case ValueType::kInt32:
        rec.AddValue(Value::Int(static_cast<int32_t>(bb.GetUint32())));
        break;
      case ValueType::kInt64:
        rec.AddValue(Value::BigInt(static_cast<int64_t>(bb.GetUint64())));
        break;
      case ValueType::kFloat: {
        const uint32_t bits = bb.GetUint32();
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        rec.AddValue(Value::Float(f));
        break;
      }
      case ValueType::kDouble: {
        const uint64_t bits = bb.GetUint64();
        double d = 0.0;
        std::memcpy(&d, &bits, sizeof(d));
        rec.AddValue(Value::Double(d));
        break;
      }
      case ValueType::kVarchar:
      case ValueType::kChar:
        rec.AddValue(Value::Varchar(bb.GetString()));   // 读「u16 长度 + 字节」
        break;
      default:
        return Status::Error(StatusCode::kTypeMismatch, "不支持的类型");
    }
  }

  *out = std::move(rec);
  return Status::OK();
}

}  // namespace cella::storage
