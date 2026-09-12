// json.h —— 极简 JSON 值与编解码（零第三方依赖）。
//
// 定位：序列化为主、解析为辅（解析只用于请求体，结构固定且简单）。
// 设计取舍：
//   * 对象保持插入序（vector 而非 map），输出稳定、可读、利于测试断言；
//   * 数值双通道：整数走 int64（rowid 等大整数不丢精度），其余走 double；
//   * 中文直接输出 UTF-8 原文，不转 \uXXXX（可读、省字节）；
//   * 解析带深度上限，防恶意请求体打爆栈。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cella::client {

class JsonValue {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  JsonValue() = default;

  static JsonValue Null();
  static JsonValue Bool(bool b);
  static JsonValue Int(std::int64_t v);
  static JsonValue Real(double v);
  static JsonValue Str(std::string s);
  static JsonValue Arr();
  static JsonValue Obj();

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::kNull; }
  bool IsObject() const { return type_ == Type::kObject; }
  bool IsArray() const { return type_ == Type::kArray; }
  bool IsString() const { return type_ == Type::kString; }
  bool IsNumber() const { return type_ == Type::kNumber; }

  bool AsBool() const { return type_ == Type::kBool ? bool_ : false; }
  bool IsInt() const { return type_ == Type::kNumber && is_int_; }
  std::int64_t AsInt(std::int64_t def = 0) const;
  double AsDouble(double def = 0) const;
  const std::string& AsString() const { return IsString() ? str_ : empty_; }

  // ── 数组 ──
  void Push(JsonValue v) { type_ = Type::kArray; arr_.push_back(std::move(v)); }
  const std::vector<JsonValue>& items() const { return arr_; }
  size_t size() const { return arr_.size(); }

  // ── 对象（保持插入序；Set 已存在则原位覆盖）──
  JsonValue& Set(const std::string& key, JsonValue v);
  const JsonValue* Find(const std::string& key) const;
  const std::vector<std::pair<std::string, JsonValue>>& members() const { return obj_; }

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  bool is_int_ = false;
  double num_ = 0.0;
  std::int64_t int_ = 0;
  std::string str_;
  std::vector<JsonValue> arr_;
  std::vector<std::pair<std::string, JsonValue>> obj_;

  static const std::string empty_;
};

// 序列化（紧凑格式；中文按 UTF-8 原文输出）
std::string JsonDump(const JsonValue& v);

// 解析请求体。失败返回 false 并在 *err 给出原因（可传 nullptr）。
bool JsonParse(const std::string& text, JsonValue* out, std::string* err);

}  // namespace cella::client
