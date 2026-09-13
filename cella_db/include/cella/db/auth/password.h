// password.h —— 口令哈希（零第三方依赖）。
//
// 存储格式（单行文本）：
//     sha256$<iterations>$<salt_hex>$<hash_hex>
//   * salt：16 字节随机值，每个用户独立（防彩虹表 / 相同口令同摘要）；
//   * hash：SHA-256 迭代 iterations 轮 —— 第 1 轮摘要 = SHA256(salt ‖ password)，
//     之后每轮 = SHA256(上一轮摘要 ‖ password)，迭代刻意拉长暴力破解成本；
//   * 校验用**常数时间比较**，不按字节提前返回，避免比较耗时泄露信息。
//
// 定位：教学强度。生产环境应换 bcrypt / scrypt / argon2（见 docs/AUTH.md）。
#pragma once

#include <cstdint>
#include <string>

namespace cella::db {

// SHA-256 摘要的十六进制小写文本（32 字节 → 64 字符）
std::string Sha256Hex(const std::string& data);

// 生成口令哈希串（自带随机盐）。iterations 必须 >= 1。
std::string HashPassword(const std::string& password, int iterations = 10000);

// 校验口令；stored 为 HashPassword 的产物。格式非法一律返回 false。
bool VerifyPassword(const std::string& password, const std::string& stored);

// 从存储串里取出迭代轮数；解析失败返回 0。
int PasswordIterations(const std::string& stored);

}  // namespace cella::db
