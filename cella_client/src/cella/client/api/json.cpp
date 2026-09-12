// json.cpp —— JsonValue 实现：序列化 + 递归下降解析。
#include "cella/client/api/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cella::client {

const std::string JsonValue::empty_;

JsonValue JsonValue::Null() { return JsonValue(); }
JsonValue JsonValue::Bool(bool b) {
  JsonValue v;
  v.type_ = Type::kBool;
  v.bool_ = b;
  return v;
}
JsonValue JsonValue::Int(std::int64_t x) {
  JsonValue v;
  v.type_ = Type::kNumber;
  v.is_int_ = true;
  v.int_ = x;
  v.num_ = static_cast<double>(x);
  return v;
}
JsonValue JsonValue::Real(double x) {
  JsonValue v;
  v.type_ = Type::kNumber;
  v.num_ = x;
  return v;
}
JsonValue JsonValue::Str(std::string s) {
  JsonValue v;
  v.type_ = Type::kString;
  v.str_ = std::move(s);
  return v;
}
JsonValue JsonValue::Arr() {
  JsonValue v;
  v.type_ = Type::kArray;
  return v;
}
JsonValue JsonValue::Obj() {
  JsonValue v;
  v.type_ = Type::kObject;
  return v;
}

std::int64_t JsonValue::AsInt(std::int64_t def) const {
  if (type_ != Type::kNumber) {
    return def;
  }
  return is_int_ ? int_ : static_cast<std::int64_t>(num_);
}
double JsonValue::AsDouble(double def) const {
  if (type_ != Type::kNumber) {
    return def;
  }
  return is_int_ ? static_cast<double>(int_) : num_;
}

JsonValue& JsonValue::Set(const std::string& key, JsonValue v) {
  type_ = Type::kObject;
  for (auto& kv : obj_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return kv.second;
    }
  }
  obj_.emplace_back(key, std::move(v));
  return obj_.back().second;
}

const JsonValue* JsonValue::Find(const std::string& key) const {
  if (type_ != Type::kObject) {
    return nullptr;
  }
  for (const auto& kv : obj_) {
    if (kv.first == key) {
      return &kv.second;
    }
  }
  return nullptr;
}

// ── 序列化 ──────────────────────────────────────────────────

namespace {

void DumpString(const std::string& s, std::string* out) {
  out->push_back('"');
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(ch);  // UTF-8 原文（含中文）
        }
    }
  }
  out->push_back('"');
}

// 选一个既短又能精确还原的浮点表示（15 → 16 → 17 位有效数字依次尝试）
std::string DumpDouble(double v) {
  if (std::isnan(v) || std::isinf(v)) {
    return "null";  // JSON 无 NaN/Inf，序列化为 null（引擎不会产生这类值）
  }
  char buf[48];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) {
      return buf;
    }
  }
  return buf;
}

void DumpValue(const JsonValue& v, std::string* out) {
  switch (v.type()) {
    case JsonValue::Type::kNull: *out += "null"; break;
    case JsonValue::Type::kBool: *out += v.AsBool() ? "true" : "false"; break;
    case JsonValue::Type::kNumber:
      if (v.IsInt()) {
        *out += std::to_string(v.AsInt());
      } else {
        *out += DumpDouble(v.AsDouble());
      }
      break;
    case JsonValue::Type::kString: DumpString(v.AsString(), out); break;
    case JsonValue::Type::kArray: {
      out->push_back('[');
      bool first = true;
      for (const auto& item : v.items()) {
        if (!first) {
          out->push_back(',');
        }
        first = false;
        DumpValue(item, out);
      }
      out->push_back(']');
      break;
    }
    case JsonValue::Type::kObject: {
      out->push_back('{');
      bool first = true;
      for (const auto& kv : v.members()) {
        if (!first) {
          out->push_back(',');
        }
        first = false;
        DumpString(kv.first, out);
        out->push_back(':');
        DumpValue(kv.second, out);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace

std::string JsonDump(const JsonValue& v) {
  std::string out;
  DumpValue(v, &out);
  return out;
}

// ── 解析 ────────────────────────────────────────────────────

namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  Parser(const std::string& text, std::string* err) : s_(text), err_(err) {}

  bool Parse(JsonValue* out) {
    SkipWs();
    if (!ParseValue(out, 0)) {
      return false;
    }
    SkipWs();
    if (pos_ != s_.size()) {
      return Fail("末尾有多余内容");
    }
    return true;
  }

 private:
  bool Fail(const char* why) {
    if (err_ != nullptr) {
      *err_ = std::string(why) + "（偏移 " + std::to_string(pos_) + "）";
    }
    return false;
  }

  void SkipWs() {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool ParseValue(JsonValue* out, int depth) {
    if (depth > kMaxDepth) {
      return Fail("嵌套过深");
    }
    if (pos_ >= s_.size()) {
      return Fail("意外的结尾");
    }
    const char c = s_[pos_];
    if (c == '{') {
      return ParseObject(out, depth);
    }
    if (c == '[') {
      return ParseArray(out, depth);
    }
    if (c == '"') {
      std::string str;
      if (!ParseString(&str)) {
        return false;
      }
      *out = JsonValue::Str(std::move(str));
      return true;
    }
    if (c == 't') {
      return ParseLit("true", JsonValue::Bool(true), out);
    }
    if (c == 'f') {
      return ParseLit("false", JsonValue::Bool(false), out);
    }
    if (c == 'n') {
      return ParseLit("null", JsonValue::Null(), out);
    }
    return ParseNumber(out);
  }

  bool ParseLit(const char* lit, JsonValue v, JsonValue* out) {
    const size_t len = std::strlen(lit);
    if (s_.compare(pos_, len, lit) != 0) {
      return Fail("非法字面量");
    }
    pos_ += len;
    *out = std::move(v);
    return true;
  }

  bool ParseObject(JsonValue* out, int depth) {
    ++pos_;  // '{'
    JsonValue obj = JsonValue::Obj();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      *out = std::move(obj);
      return true;
    }
    while (true) {
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != '"') {
        return Fail("对象键应为字符串");
      }
      std::string key;
      if (!ParseString(&key)) {
        return false;
      }
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') {
        return Fail("缺少 ':'");
      }
      ++pos_;
      SkipWs();
      JsonValue val;
      if (!ParseValue(&val, depth + 1)) {
        return false;
      }
      obj.Set(key, std::move(val));
      SkipWs();
      if (pos_ >= s_.size()) {
        return Fail("对象未闭合");
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        *out = std::move(obj);
        return true;
      }
      return Fail("对象应为 ',' 或 '}'");
    }
  }

  bool ParseArray(JsonValue* out, int depth) {
    ++pos_;  // '['
    JsonValue arr = JsonValue::Arr();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      *out = std::move(arr);
      return true;
    }
    while (true) {
      SkipWs();
      JsonValue val;
      if (!ParseValue(&val, depth + 1)) {
        return false;
      }
      arr.Push(std::move(val));
      SkipWs();
      if (pos_ >= s_.size()) {
        return Fail("数组未闭合");
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        *out = std::move(arr);
        return true;
      }
      return Fail("数组应为 ',' 或 ']'");
    }
  }

  static void AppendUtf8(unsigned cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string* out) {
    ++pos_;  // '"'
    while (true) {
      if (pos_ >= s_.size()) {
        return Fail("字符串未闭合");
      }
      const char c = s_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        ++pos_;
        continue;
      }
      ++pos_;
      if (pos_ >= s_.size()) {
        return Fail("转义序列不完整");
      }
      const char e = s_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          unsigned cp = 0;
          if (!ParseHex4(&cp)) {
            return false;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
              s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
            const size_t save = pos_;
            pos_ += 2;
            unsigned lo = 0;
            if (ParseHex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              pos_ = save;  // 非代理对：按原样输出高位代理
            }
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          return Fail("非法转义字符");
      }
    }
  }

  bool ParseHex4(unsigned* out) {
    if (pos_ + 4 > s_.size()) {
      return Fail("\\u 需要 4 位十六进制");
    }
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        return Fail("\\u 含非十六进制字符");
      }
    }
    *out = v;
    return true;
  }

  bool ParseNumber(JsonValue* out) {
    const size_t begin = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
      ++pos_;
    }
    bool digits = false;
    bool is_int = true;
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c >= '0' && c <= '9') {
        digits = true;
        ++pos_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        is_int = false;
        ++pos_;
      } else {
        break;
      }
    }
    if (!digits) {
      return Fail("非法数字");
    }
    const std::string text = s_.substr(begin, pos_ - begin);
    if (is_int) {
      errno = 0;
      char* end = nullptr;
      const long long v = std::strtoll(text.c_str(), &end, 10);
      if (errno == 0 && end != nullptr && *end == '\0') {
        *out = JsonValue::Int(static_cast<std::int64_t>(v));
        return true;
      }
    }
    *out = JsonValue::Real(std::strtod(text.c_str(), nullptr));
    return true;
  }

  const std::string& s_;
  std::string* err_;
  size_t pos_ = 0;
};

}  // namespace

bool JsonParse(const std::string& text, JsonValue* out, std::string* err) {
  if (err != nullptr) {
    err->clear();
  }
  Parser p(text, err);
  JsonValue v;
  if (!p.Parse(&v)) {
    return false;
  }
  *out = std::move(v);
  return true;
}

}  // namespace cella::client
