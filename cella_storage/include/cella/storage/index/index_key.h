#pragma once
#include <cstdint>
#include <string>

#include "cella/storage/common/value.h"

namespace cella::storage {

// ─────────────────────────────────────────────────────────────────────────
// 索引键编码 —— B+ 树只按「字节字典序」比较，因此必须把各种类型的值编码成
// 「编码后字典序 == 逻辑序」的字节串。这是整个索引正确性的基石。
//
// 各类型的编码策略：
//
//   INT32 / INT64
//     有符号整数二进制里负数最高位是 1，直接大端比较会把负数排到正数后面。
//     → 翻转符号位后按大端写出，得到保序编码。
//
//   FLOAT / DOUBLE (IEEE754)
//     正数：翻转符号位；负数：全部位取反。标准浮点保序编码。
//
//   VARCHAR / CHAR / TEXT
//     UTF-8 字节序即字典序，直接写字节；内容中的 0x00 转义为 0x00 0xFF，
//     结尾用 0x00 0x00 标记，使「前缀串排在长子串之前」。
//
//   NULL：0x00 0x01 标记，排在最前（与 ORDER BY 惯例一致）。
//
// 键的组成： [列值编码] [+ 行定位 5B（仅叶子）]
//   非唯一索引里同一列值可对应多行 —— 行定位附加在键尾，使整键唯一。
//   内部节点的分隔键**不含**行定位，因此比较时对内部键使用前缀比较。
//
// 解码：键本身不能自描述类型（4B 既可能是 INT32 也可能是 FLOAT），
// 因此解码函数**显式接收列类型**。这是有意的取舍：编码保持紧凑，
// 解码由调用方（知道索引定义在哪个列上）提供类型。
// ─────────────────────────────────────────────────────────────────────────

// 编码一个列值（不含行定位）。输出追加到 out。
void EncodeIndexColumn(const Value& v, std::string* out);

// 编码「列值 + 行定位」，得到叶子里完整的键。
std::string EncodeLeafKey(const Value& v, page_id_t page_id, uint8_t slot_id);

// 解码叶子键的列值部分（按给定列类型解释）。
bool DecodeLeafKeyColumn(const std::string& key, ValueType type, Value* out);

// 解码叶子键的行定位部分。
bool DecodeLeafKeyRid(const std::string& key, page_id_t* page_id, uint8_t* slot_id);

// 去掉叶子键尾部的行定位，得到「列值键」（内部节点分隔键形态）。
std::string StripLeafRowId(const std::string& leaf_key);

// 比较两个键。b_is_prefix = true 表示 b 是「只含列值部分」的内部键，
// 与 a（列值 + 行定位）的对应前缀比较。
int CompareIndexKey(const std::string& a, const std::string& b, bool b_is_prefix);

// 键的尾部行定位长度（供空间估算）
constexpr size_t kIndexLeafRidBytes = 5;

}  // namespace cella::storage
