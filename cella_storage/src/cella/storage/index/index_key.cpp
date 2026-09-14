#include "cella/storage/index/index_key.h"

#include <cstring>

namespace cella::storage {

namespace {

// ── 保序整数编码 ────────────────────────────────────────────
void PutOrderedU32(uint32_t v, std::string* out) {
  const uint32_t x = v ^ 0x80000000u;          // 翻转符号位
  for (int i = 3; i >= 0; --i) {
    out->push_back(static_cast<char>((x >> (8 * i)) & 0xFFu));
  }
}

void PutOrderedU64(uint64_t v, std::string* out) {
  const uint64_t x = v ^ 0x8000000000000000ull;
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<char>((x >> (8 * i)) & 0xFFu));
  }
}

// ── 保序浮点编码 ────────────────────────────────────────────
void PutOrderedFloat(float f, std::string* out) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  const uint32_t x = ((bits & 0x80000000u) != 0) ? ~bits : (bits | 0x80000000u);
  for (int i = 3; i >= 0; --i) {
    out->push_back(static_cast<char>((x >> (8 * i)) & 0xFFu));
  }
}

void PutOrderedDouble(double d, std::string* out) {
  uint64_t bits = 0;
  std::memcpy(&bits, &d, sizeof(bits));
  const uint64_t x =
      ((bits & 0x8000000000000000ull) != 0) ? ~bits : (bits | 0x8000000000000000ull);
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<char>((x >> (8 * i)) & 0xFFu));
  }
}

// ── 保序字符串编码 ──────────────────────────────────────────
void PutEscapedString(const std::string& s, std::string* out) {
  for (const char ch : s) {
    if (static_cast<unsigned char>(ch) == 0x00) {
      out->push_back(static_cast<char>(0x00));
      out->push_back(static_cast<char>(0xFF));
    } else {
      out->push_back(ch);
    }
  }
  out->push_back(static_cast<char>(0x00));
  out->push_back(static_cast<char>(0x00));
}

constexpr unsigned char kTagNull = 0x00;
constexpr unsigned char kTagFalse = 0x01;
constexpr unsigned char kTagTrue = 0x02;

uint32_t ReadU32BE(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

uint64_t ReadU64BE(const char* p) {
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) {
    x = (x << 8) | static_cast<unsigned char>(p[i]);
  }
  return x;
}

// 解出「列值键」的长度（不含行定位）。返回 0 表示无法解析。
// 只有变长类型（VARCHAR/CHAR/TEXT）需要这一步；定长类型按长度直接切。
size_t ColumnPayloadLen(const std::string& key, ValueType type) {
  const size_t avail = (key.size() >= kIndexLeafRidBytes) ? key.size() - kIndexLeafRidBytes : key.size();
  switch (type) {
    case ValueType::kBool:
    case ValueType::kNull:
      return avail >= 1 ? 1 : 0;
    case ValueType::kInt32:
    case ValueType::kFloat:
      return avail >= 4 ? 4 : 0;
    case ValueType::kInt64:
    case ValueType::kDouble:
      return avail >= 8 ? 8 : 0;
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate:
      // 扫到 0x00 0x00 结尾标记（注意 0x00 0xFF 是转义）
      for (size_t i = 0; i + 1 < avail; ++i) {
        if (static_cast<unsigned char>(key[i]) == 0x00) {
          if (static_cast<unsigned char>(key[i + 1]) == 0xFF) {
            ++i;   // 跳过转义对
            continue;
          }
          if (static_cast<unsigned char>(key[i + 1]) == 0x00) {
            return i + 2;   // 含结尾标记
          }
        }
      }
      return 0;
  }
  return 0;
}

// ── 按位置解出一列的编码长度与值 ────────────────────────────
// data/avail 是该列的可用窗口（复合键里就是「从这里到行定位之间」）。
// 成功时 *len 为该列消耗的字节数、*out 为解码值；失败返回 false。
// 注意：NULL 不在这里判 —— 复合键用位图判定；单列便捷接口在
// DecodeLeafKeyColumn 里先看首字节标记（与历史行为一致）。
bool ParseColumnAt(const char* data, size_t avail, ValueType type, size_t* len, Value* out) {
  switch (type) {
    case ValueType::kBool:
      if (avail < 1) {
        return false;
      }
      *len = 1;
      *out = Value::Bool(static_cast<unsigned char>(data[0]) == kTagTrue);
      return true;
    case ValueType::kNull:
      if (avail < 1) {
        return false;
      }
      *len = 1;
      *out = Value::Null();
      return true;
    case ValueType::kInt32:
      if (avail < 4) {
        return false;
      }
      *len = 4;
      *out = Value::Int(static_cast<int32_t>(ReadU32BE(data) ^ 0x80000000u));
      return true;
    case ValueType::kInt64:
      if (avail < 8) {
        return false;
      }
      *len = 8;
      *out = Value::BigInt(static_cast<int64_t>(ReadU64BE(data) ^ 0x8000000000000000ull));
      return true;
    case ValueType::kFloat: {
      if (avail < 4) {
        return false;
      }
      *len = 4;
      const uint32_t x = ReadU32BE(data);
      const uint32_t bits = ((x & 0x80000000u) != 0) ? (x & 0x7FFFFFFFu) | 0x80000000u : ~x;
      float f = 0.0f;
      std::memcpy(&f, &bits, sizeof(f));
      *out = Value::Float(f);
      return true;
    }
    case ValueType::kDouble: {
      if (avail < 8) {
        return false;
      }
      *len = 8;
      const uint64_t x = ReadU64BE(data);
      const uint64_t bits =
          ((x & 0x8000000000000000ull) != 0) ? (x & 0x7FFFFFFFFFFFFFFFull) | 0x8000000000000000ull : ~x;
      double d = 0.0;
      std::memcpy(&d, &bits, sizeof(d));
      *out = Value::Double(d);
      return true;
    }
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate: {
      // 自定界：扫 0x00 0x00 结尾标记（0x00 0xFF 是转义对，跳过）
      for (size_t i = 0; i + 1 < avail; ++i) {
        if (static_cast<unsigned char>(data[i]) == 0x00) {
          if (static_cast<unsigned char>(data[i + 1]) == 0xFF) {
            ++i;
            continue;
          }
          if (static_cast<unsigned char>(data[i + 1]) == 0x00) {
            const size_t n = i + 2;
            std::string s;
            const size_t end = n - 2;
            for (size_t j = 0; j < end; ++j) {
              if (static_cast<unsigned char>(data[j]) == 0x00 &&
                  j + 1 < end && static_cast<unsigned char>(data[j + 1]) == 0xFF) {
                s.push_back('\0');
                ++j;
                continue;
              }
              s.push_back(data[j]);
            }
            *len = n;
            *out = Value::Varchar(s);
            return true;
          }
        }
      }
      return false;
    }
  }
  return false;
}

}  // namespace

void EncodeIndexColumn(const Value& v, std::string* out) {
  if (v.IsNull()) {
    out->push_back(static_cast<char>(kTagNull));
    out->push_back(static_cast<char>(0x01));
    return;
  }
  switch (v.type) {
    case ValueType::kBool:
      out->push_back(static_cast<char>(v.bool_val ? kTagTrue : kTagFalse));
      break;
    case ValueType::kInt32:
      PutOrderedU32(static_cast<uint32_t>(v.int32_val), out);
      break;
    case ValueType::kInt64:
      PutOrderedU64(static_cast<uint64_t>(v.int64_val), out);
      break;
    case ValueType::kFloat:
      PutOrderedFloat(v.float_val, out);
      break;
    case ValueType::kDouble:
      PutOrderedDouble(v.double_val, out);
      break;
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate:
      PutEscapedString(v.str_val, out);
      break;
    case ValueType::kNull:
      out->push_back(static_cast<char>(kTagNull));
      out->push_back(static_cast<char>(0x01));
      break;
  }
}

std::string EncodeLeafKey(const Value& v, page_id_t page_id, uint8_t slot_id) {
  std::string key;
  EncodeIndexColumn(v, &key);
  key.push_back(static_cast<char>((page_id >> 24) & 0xFFu));
  key.push_back(static_cast<char>((page_id >> 16) & 0xFFu));
  key.push_back(static_cast<char>((page_id >> 8) & 0xFFu));
  key.push_back(static_cast<char>(page_id & 0xFFu));
  key.push_back(static_cast<char>(slot_id));
  return key;
}

size_t NullBitmapBytes(size_t column_count) {
  // 单列不加位图（与历史格式逐字节兼容）；复合键才需要。
  if (column_count < 2) {
    return 0;
  }
  return (column_count + 7) / 8;
}

namespace {

// 写出 NULL 位图（宽度 = vals.size()，第 i 位 = 第 i 列是否 NULL）。
// 复合键编码与部分前缀编码共用（位图宽度必须与整键一致）。
void AppendNullBitmap(const std::vector<Value>& vals, std::string* out) {
  const size_t bytes = NullBitmapBytes(vals.size());
  for (size_t b = 0; b < bytes; ++b) {
    unsigned char mask = 0;
    for (size_t i = b * 8; i < vals.size() && i < (b + 1) * 8; ++i) {
      if (vals[i].IsNull()) {
        mask |= static_cast<unsigned char>(1u << (i % 8));
      }
    }
    out->push_back(static_cast<char>(mask));
  }
}

}  // namespace

std::string EncodeColumnKeys(const std::vector<Value>& vals) {
  std::string key;
  // 复合键：先写 NULL 位图（第 i 位 = 第 i 列是否 NULL），再逐列拼接。
  // 位图让解码端的列边界完全确定 —— 否则 NULL 标记（00 01，2 字节）与
  // 定宽列的值编码可能共享前缀（例：INT32 0x80017879 编码 = 00 01 78 79），
  // 两个不同元组会拼出同一个键。位图是权威判定，歧义从机制上消失。
  if (vals.size() >= 2) {
    AppendNullBitmap(vals, &key);
  }
  for (const Value& v : vals) {
    EncodeIndexColumn(v, &key);
  }
  return key;
}

std::string EncodeColumnPrefix(const std::vector<Value>& prefix_vals, size_t total_columns) {
  std::string key;
  if (total_columns >= 2) {
    // 位图宽度 = 索引总列数（与整键对齐）。**未给出的前缀列位一律写 0**
    //（按非 NULL 占位）：整键里非 NULL 列的位就是 0，只有这样才能与
    // 「前缀列相同」的大多数键对齐。（补成 NULL 占位会把未给列的位也
    // 置 1，前缀就再也匹配不上非 NULL 键 —— 这正是第一版实现踩的坑。）
    const size_t bytes = (total_columns + 7) / 8;
    for (size_t b = 0; b < bytes; ++b) {
      unsigned char mask = 0;
      for (size_t i = b * 8; i < prefix_vals.size() && i < (b + 1) * 8; ++i) {
        if (prefix_vals[i].IsNull()) {
          mask |= static_cast<unsigned char>(1u << (i % 8));
        }
      }
      key.push_back(static_cast<char>(mask));
    }
  }
  // 拼接体只写前缀列
  for (const Value& v : prefix_vals) {
    EncodeIndexColumn(v, &key);
  }
  return key;
}

std::string EncodeLeafKeyColumns(const std::vector<Value>& vals, page_id_t page_id,
                                 uint8_t slot_id) {
  std::string key = EncodeColumnKeys(vals);
  key.push_back(static_cast<char>((page_id >> 24) & 0xFFu));
  key.push_back(static_cast<char>((page_id >> 16) & 0xFFu));
  key.push_back(static_cast<char>((page_id >> 8) & 0xFFu));
  key.push_back(static_cast<char>(page_id & 0xFFu));
  key.push_back(static_cast<char>(slot_id));
  return key;
}

bool DecodeLeafKeyColumn(const std::string& key, ValueType type, Value* out) {
  if (key.empty()) {
    return false;
  }
  // 先看首字节标记：NULL / BOOL 的编码与列类型无关（NULL 只占 2 字节），
  // 因此必须在「按类型算列值长度」之前判断，否则 NULL 会被长度检查判为非法。
  const unsigned char tag = static_cast<unsigned char>(key[0]);
  if (tag == kTagNull) {
    *out = Value::Null();
    return true;
  }
  if (type == ValueType::kBool) {
    *out = Value::Bool(tag == kTagTrue);
    return true;
  }

  // 非 NULL：按列类型计算列值部分的长度
  const size_t n = ColumnPayloadLen(key, type);
  if (n == 0) {
    return false;
  }
  switch (type) {
    case ValueType::kInt32:
      *out = Value::Int(static_cast<int32_t>(ReadU32BE(key.data()) ^ 0x80000000u));
      return true;
    case ValueType::kInt64: {
      const uint64_t x = ReadU64BE(key.data()) ^ 0x8000000000000000ull;
      *out = Value::BigInt(static_cast<int64_t>(x));
      return true;
    }
    case ValueType::kFloat: {
      const uint32_t x = ReadU32BE(key.data());
      const uint32_t bits = ((x & 0x80000000u) != 0) ? (x & 0x7FFFFFFFu) | 0x80000000u : ~x;
      float f = 0.0f;
      std::memcpy(&f, &bits, sizeof(f));
      *out = Value::Float(f);
      return true;
    }
    case ValueType::kDouble: {
      const uint64_t x = ReadU64BE(key.data());
      const uint64_t bits =
          ((x & 0x8000000000000000ull) != 0) ? (x & 0x7FFFFFFFFFFFFFFFull) | 0x8000000000000000ull : ~x;
      double d = 0.0;
      std::memcpy(&d, &bits, sizeof(d));
      *out = Value::Double(d);
      return true;
    }
    case ValueType::kBool:
      *out = Value::Bool(tag == kTagTrue);
      return true;
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate: {
      std::string s;
      // key[0..n) 是列值键（含结尾标记）；去掉尾部 2 字节标记
      const size_t end = (n >= 2) ? n - 2 : 0;
      for (size_t i = 0; i < end; ++i) {
        if (static_cast<unsigned char>(key[i]) == 0x00) {
          if (i + 1 < end && static_cast<unsigned char>(key[i + 1]) == 0xFF) {
            s.push_back('\0');
            ++i;
            continue;
          }
        }
        s.push_back(key[i]);
      }
      *out = Value::Varchar(s);
      return true;
    }
    case ValueType::kNull:
      *out = Value::Null();
      return true;
  }
  return false;
}

bool DecodeLeafKeyColumns(const std::string& key, const std::vector<ValueType>& types,
                          std::vector<Value>* out) {
  out->clear();
  if (types.empty()) {
    return false;
  }
  // 列值部分 = 去掉尾部行定位。不足 5 字节的键视为「纯列值键」（内部节点
  // 分隔键形态），此时列值部分就是整键 —— 宽容处理，方便诊断工具复用。
  const size_t col_total =
      (key.size() >= kIndexLeafRidBytes) ? key.size() - kIndexLeafRidBytes : key.size();
  const char* data = key.data();
  const size_t n = types.size();
  size_t off = 0;

  // 列数 ≥ 2：开头是 NULL 位图（编码端 EncodeColumnKeys 的对称操作）。
  std::vector<bool> is_null(n, false);
  if (n >= 2) {
    const size_t bmp = NullBitmapBytes(n);
    if (col_total < bmp) {
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      const unsigned char byte = static_cast<unsigned char>(data[i / 8]);
      is_null[i] = (byte & (1u << (i % 8))) != 0;
    }
    off = bmp;
  }

  out->resize(n);
  for (size_t i = 0; i < n; ++i) {
    if (is_null[i]) {
      // 位图判定为 NULL：编码端写入了 2 字节 00 01 填充（保持排序位次），
      // 这里按已知长度跳过 —— 列边界由位图权威决定，不存在歧义。
      if (off + 2 > col_total) {
        return false;
      }
      out->at(i) = Value::Null();
      off += 2;
      continue;
    }
    size_t len = 0;
    if (!ParseColumnAt(data + off, col_total - off, types[i], &len, &out->at(i))) {
      return false;
    }
    off += len;
  }
  // 必须完整消费列值部分：否则「错位一列仍合法」的键会被静默接受。
  if (off != col_total) {
    return false;
  }
  return true;
}

bool DecodeLeafKeyRid(const std::string& key, page_id_t* page_id, uint8_t* slot_id) {
  if (key.size() < kIndexLeafRidBytes) {
    return false;
  }
  const size_t base = key.size() - kIndexLeafRidBytes;
  *page_id = ReadU32BE(key.data() + base);
  *slot_id = static_cast<uint8_t>(key[base + 4]);
  return true;
}

std::string StripLeafRowId(const std::string& leaf_key) {
  if (leaf_key.size() < kIndexLeafRidBytes) {
    return leaf_key;
  }
  return leaf_key.substr(0, leaf_key.size() - kIndexLeafRidBytes);
}

size_t MaxEncodedColumnLen(ValueType type, uint16_t max_len) {
  switch (type) {
    case ValueType::kBool:
    case ValueType::kNull:
      return 1;
    case ValueType::kInt32:
    case ValueType::kFloat:
      return 4;
    case ValueType::kInt64:
    case ValueType::kDouble:
      return 8;
    case ValueType::kVarchar:
    case ValueType::kChar:
    case ValueType::kDate:
      // 最坏情况：内容每个字节都是 0x00 → 转义成 0x00 0xFF（翻倍），
      // 再加 2 字节结尾标记。max_len 为 0（未声明）按 255 兜底。
      return static_cast<size_t>(max_len > 0 ? max_len : 255) * 2 + 2;
  }
  return 8;
}

int CompareIndexKey(const std::string& a, const std::string& b, bool b_is_prefix) {
  if (!b_is_prefix) {
    const int c = a.compare(b);
    return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
  }
  const size_t n = b.size() < a.size() ? b.size() : a.size();
  const int c = a.compare(0, n, b, 0, n);
  if (c != 0) {
    return (c < 0) ? -1 : 1;
  }
  if (a.size() == b.size()) {
    return 0;
  }
  return a.size() > b.size() ? 1 : -1;
}

}  // namespace cella::storage
