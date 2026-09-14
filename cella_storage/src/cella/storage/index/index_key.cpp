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
