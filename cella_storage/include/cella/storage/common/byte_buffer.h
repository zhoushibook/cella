#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 小端（little-endian）安全的整数编解码。
//
// 为什么需要它：不同机器字节序可能不同（x86 是小端、部分 ARM 可切大端）。
// 数据要落盘、要跨机器读，就必须固定一种字节序。这里统一「小端」：
// 一个 uint32 写进缓冲区时，低字节在前、高字节在后。
//
// 为什么逐字节移位、而不是直接 memcpy 一个整数：
//   直接 memcpy 会把「当前机器的字节序」原样写进去，换台字节序不同的机器就乱了。
//   逐字节移位则与机器字节序无关，保证无论在哪台机器上跑，落盘格式都一样。
//   （约束 #5：禁止 memcpy 结构体；这里的 memcpy 只用于 float/double 的位重解释，
//     不涉及多字节整数的字节序问题。）
//
// 两组 API：
//   ① 固定偏移函数：PutUint16(char*, v) / GetUint16(const char*)，直接读写 char* 缓冲区，
//      用于页头、MetaPage、槽项这类「已知偏移」的字段。
//   ② ByteBuffer 流式类：像写流一样逐个 Put/Get，内部维护读/写位置，用于记录序列化。
// ─────────────────────────────────────────────────────────────────────────

// —— 固定偏移写入（往 dst 开始的缓冲区写小端整数）——
inline void PutUint16(char* dst, uint16_t v) {
  dst[0] = static_cast<char>(v & 0xFFu);        // 低字节在前
  dst[1] = static_cast<char>((v >> 8) & 0xFFu); // 高字节在后
}

inline void PutUint32(char* dst, uint32_t v) {
  dst[0] = static_cast<char>(v & 0xFFu);
  dst[1] = static_cast<char>((v >> 8) & 0xFFu);
  dst[2] = static_cast<char>((v >> 16) & 0xFFu);
  dst[3] = static_cast<char>((v >> 24) & 0xFFu);
}

inline void PutUint64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((v >> (8 * i)) & 0xFFull);
  }
}

// —— 固定偏移读取（从 src 开始的缓冲区读小端整数）——
inline uint16_t GetUint16(const char* src) {
  return static_cast<uint16_t>(
      static_cast<uint16_t>(static_cast<uint8_t>(src[0])) |
      static_cast<uint16_t>(static_cast<uint8_t>(src[1]) << 8));
}

inline uint32_t GetUint32(const char* src) {
  return static_cast<uint32_t>(static_cast<uint8_t>(src[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(src[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(src[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(src[3])) << 24);
}

inline uint64_t GetUint64(const char* src) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(src[i])) << (8 * i);
  }
  return v;
}

// ─────────────────────────────────────────────────────────────────────────
// ByteBuffer：流式缓冲区（记录序列化用）。
// 写：一串 Put* 往 data_ 末尾追加字节；读：一串 Get* 从 pos_ 往后读并前移 pos_。
// ─────────────────────────────────────────────────────────────────────────
class ByteBuffer {
 public:
  explicit ByteBuffer(std::vector<char> data = {}) : data_(std::move(data)) {}

  // —— 写入（追加到末尾）——
  void PutUint8(uint8_t v) { data_.push_back(static_cast<char>(v)); }

  void PutUint16(uint16_t v) {
    char b[2];
    cella::storage::PutUint16(b, v);   // 复用固定偏移函数（小端）
    data_.insert(data_.end(), b, b + 2);
  }

  void PutUint32(uint32_t v) {
    char b[4];
    cella::storage::PutUint32(b, v);
    data_.insert(data_.end(), b, b + 4);
  }

  void PutUint64(uint64_t v) {
    char b[8];
    cella::storage::PutUint64(b, v);
    data_.insert(data_.end(), b, b + 8);
  }

  void PutBytes(const char* p, size_t n) { data_.insert(data_.end(), p, p + n); }

  // 写变长字符串：先写 u16 长度前缀，再写字节
  void PutString(const std::string& s) {
    PutUint16(static_cast<uint16_t>(s.size()));
    PutBytes(s.data(), s.size());
  }

  // —— 读取（从 pos_ 往后读，并前移 pos_）——
  uint8_t GetUint8() { return static_cast<uint8_t>(data_[pos_++]); }

  uint16_t GetUint16() {
    uint16_t v = cella::storage::GetUint16(data_.data() + pos_);
    pos_ += 2;
    return v;
  }

  uint32_t GetUint32() {
    uint32_t v = cella::storage::GetUint32(data_.data() + pos_);
    pos_ += 4;
    return v;
  }

  uint64_t GetUint64() {
    uint64_t v = cella::storage::GetUint64(data_.data() + pos_);
    pos_ += 8;
    return v;
  }

  std::string GetString() {
    const uint16_t len = GetUint16();          // 先读长度前缀
    std::string s(data_.data() + pos_, static_cast<size_t>(len));
    pos_ += len;
    return s;
  }

  std::vector<char> GetBytes(size_t n) {
    std::vector<char> v(data_.data() + pos_, data_.data() + pos_ + n);
    pos_ += n;
    return v;
  }

  const std::vector<char>& data() const { return data_; }
  std::vector<char>& data() { return data_; }
  size_t size() const { return data_.size(); }
  size_t pos() const { return pos_; }
  void Reset() { pos_ = 0; }                    // 从头再读一遍

 private:
  std::vector<char> data_;   // 字节数据
  size_t pos_ = 0;           // 当前读取位置
};

}  // namespace cella::storage
