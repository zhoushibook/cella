#include "cella/db/auth/password.h"

#include <array>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

namespace cella::db {
namespace {

// ── SHA-256（FIPS 180-4）──────────────────────────────────────
constexpr uint32_t kRoundK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t len) {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  std::vector<uint8_t> msg(data, data + len);
  const uint64_t bit_len = static_cast<uint64_t>(len) * 8u;
  msg.push_back(0x80u);
  while (msg.size() % 64u != 56u) {
    msg.push_back(0x00u);
  }
  for (int i = 7; i >= 0; --i) {
    msg.push_back(static_cast<uint8_t>((bit_len >> (static_cast<unsigned>(i) * 8u)) & 0xFFu));
  }

  for (size_t off = 0; off < msg.size(); off += 64u) {
    uint32_t w[64];
    for (size_t i = 0; i < 16u; ++i) {
      w[i] = (static_cast<uint32_t>(msg[off + i * 4u]) << 24) |
             (static_cast<uint32_t>(msg[off + i * 4u + 1u]) << 16) |
             (static_cast<uint32_t>(msg[off + i * 4u + 2u]) << 8) |
             static_cast<uint32_t>(msg[off + i * 4u + 3u]);
    }
    for (size_t i = 16u; i < 64u; ++i) {
      const uint32_t s0 = Rotr(w[i - 15u], 7u) ^ Rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
      const uint32_t s1 = Rotr(w[i - 2u], 17u) ^ Rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
      w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (size_t i = 0; i < 64u; ++i) {
      const uint32_t big_s1 = Rotr(e, 6u) ^ Rotr(e, 11u) ^ Rotr(e, 25u);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t t1 = hh + big_s1 + ch + kRoundK[i] + w[i];
      const uint32_t big_s0 = Rotr(a, 2u) ^ Rotr(a, 13u) ^ Rotr(a, 22u);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = big_s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < 8u; ++i) {
    out[i * 4u] = static_cast<uint8_t>((h[i] >> 24) & 0xFFu);
    out[i * 4u + 1u] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
    out[i * 4u + 2u] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
    out[i * 4u + 3u] = static_cast<uint8_t>(h[i] & 0xFFu);
  }
  return out;
}

std::string ToHex(const uint8_t* p, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2u);
  for (size_t i = 0; i < n; ++i) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 0x0Fu];
  }
  return s;
}

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool FromHex(const std::string& hex, std::vector<uint8_t>* out) {
  out->clear();
  if (hex.size() % 2u != 0u) {
    return false;
  }
  out->reserve(hex.size() / 2u);
  for (size_t i = 0; i < hex.size(); i += 2u) {
    const int hi = HexNibble(hex[i]);
    const int lo = HexNibble(hex[i + 1u]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string RandomSaltHex(size_t bytes) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);
  std::vector<uint8_t> buf(bytes);
  for (size_t i = 0; i < bytes; ++i) {
    buf[i] = static_cast<uint8_t>(dist(gen));
  }
  return ToHex(buf.data(), buf.size());
}

// 一轮派生：SHA256(prev ‖ password)
std::vector<uint8_t> DeriveOnce(const std::vector<uint8_t>& prev, const std::string& password) {
  std::vector<uint8_t> buf = prev;
  buf.insert(buf.end(), password.begin(), password.end());
  const std::array<uint8_t, 32> d = Sha256(buf.data(), buf.size());
  return std::vector<uint8_t>(d.begin(), d.end());
}

std::vector<uint8_t> Derive(const std::vector<uint8_t>& salt, const std::string& password,
                            int iterations) {
  std::vector<uint8_t> cur = salt;
  for (int i = 0; i < iterations; ++i) {
    cur = DeriveOnce(cur, password);
  }
  return cur;
}

bool ConstantTimeEquals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff = static_cast<uint8_t>(diff | static_cast<uint8_t>(a[i] ^ b[i]));
  }
  return diff == 0;
}

struct ParsedStored {
  int iterations = 0;
  std::vector<uint8_t> salt;
  std::vector<uint8_t> hash;
};

bool ParseStored(const std::string& stored, ParsedStored* out) {
  // sha256$<iter>$<salt_hex>$<hash_hex>
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t pos = stored.find('$', start);
    if (pos == std::string::npos) {
      parts.push_back(stored.substr(start));
      break;
    }
    parts.push_back(stored.substr(start, pos - start));
    start = pos + 1u;
  }
  if (parts.size() != 4u || parts[0] != "sha256") {
    return false;
  }
  const int iter = std::atoi(parts[1].c_str());
  if (iter < 1) {
    return false;
  }
  if (!FromHex(parts[2], &out->salt) || !FromHex(parts[3], &out->hash)) {
    return false;
  }
  if (out->salt.empty() || out->hash.empty()) {
    return false;
  }
  out->iterations = iter;
  return true;
}

constexpr int kSaltBytes = 16;

}  // namespace

std::string Sha256Hex(const std::string& data) {
  const std::array<uint8_t, 32> d =
      Sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return ToHex(d.data(), d.size());
}

std::string HashPassword(const std::string& password, int iterations) {
  if (iterations < 1) {
    iterations = 1;
  }
  const std::string salt_hex = RandomSaltHex(static_cast<size_t>(kSaltBytes));
  std::vector<uint8_t> salt;
  (void)FromHex(salt_hex, &salt);
  const std::vector<uint8_t> digest = Derive(salt, password, iterations);
  return "sha256$" + std::to_string(iterations) + "$" + salt_hex + "$" + ToHex(digest.data(), digest.size());
}

bool VerifyPassword(const std::string& password, const std::string& stored) {
  ParsedStored p;
  if (!ParseStored(stored, &p)) {
    return false;
  }
  const std::vector<uint8_t> digest = Derive(p.salt, password, p.iterations);
  return ConstantTimeEquals(digest, p.hash);
}

int PasswordIterations(const std::string& stored) {
  ParsedStored p;
  return ParseStored(stored, &p) ? p.iterations : 0;
}

}  // namespace cella::db
