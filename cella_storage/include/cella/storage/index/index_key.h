#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
//     因此 00 01（NULL 标记）不可能出现在字符串编码内部（内容里的 00
//     必转成 00 FF），字符串列是**自定界**的。
//
//   NULL：0x00 0x01 标记，排在最前（与 ORDER BY 惯例一致）。
//
// 键的组成： [列值编码] [+ 行定位 5B（仅叶子）]
//   非唯一索引里同一列值可对应多行 —— 行定位附加在键尾，使整键唯一。
//   内部节点的分隔键**不含**行定位，因此比较时对内部键使用前缀比较。
//
// ── 复合键（多列）与 NULL 位图 ────────────────────────────────
//
// 复合键 = 各列 EncodeIndexColumn 依次拼接。定宽类型（INT/FLOAT 的翻转
// 符号位大端）定长、字符串自定界，拼接后**列边界**理论可解析。但有一个
// 必须堵上的歧义：NULL 标记 00 01（2 字节）与定宽列的值编码可能共享
// 前缀 —— 例如 INT32 值 -2147417223（0x80017879）编码恰为 00 01 78 79，
// 于是 (NULL, "xy") 与 (-2147417223, "") 会拼出**完全相同**的键。
//
// 解法：列数 ≥ 2 的键在最前面加一个 **NULL 位图**（ceil(n/8) 字节，
// 第 i 位 = 第 i 列是否 NULL）。解码以位图为准，列边界完全确定，
// 编码恢复单射（不同元组必得不同键）。NULL 列仍写出 00 01 填充，
// 保持「NULL 排在非空值之前」的排序位次。
//
// 单列键（列数 == 1）**不加位图**，字节格式与历史版本逐字节一致 ——
// 既有数据文件与测试不受影响。
//
// 解码：键本身不能自描述类型（4B 既可能是 INT32 也可能是 FLOAT），
// 因此解码函数**显式接收列类型序列**（类型来自索引定义，头注释约定不变）。
// ─────────────────────────────────────────────────────────────────────────

// 编码一个列值（不含行定位）。输出追加到 out。
void EncodeIndexColumn(const Value& v, std::string* out);

// 编码「列值 + 行定位」，得到叶子里完整的键（单列便捷接口，等价于
// EncodeLeafKeyColumns 的列数为 1 特例）。
std::string EncodeLeafKey(const Value& v, page_id_t page_id, uint8_t slot_id);

// ── 复合键 ──────────────────────────────────────────────────
// 列值键（无行定位）：各列依次拼接；列数 ≥ 2 时前缀加 NULL 位图。
// 用途：内部节点分隔键、唯一性判定的「元组前缀」。
std::string EncodeColumnKeys(const std::vector<Value>& vals);

// 部分前缀（最左前缀扫描用）：位图按**索引总列数** total_columns 编码
//（前缀列的 NULL 位照写，未给出的列按「非 NULL」处理），再拼前缀各列编码。
// 注意：位图宽度必须与整键一致，否则前缀匹配会错位；代价是「未前缀列
// 含 NULL」的行落在另一个位图区，不被部分前缀命中（全键等值不受影响）。
std::string EncodeColumnPrefix(const std::vector<Value>& prefix_vals, size_t total_columns);

// 叶子键 = EncodeColumnKeys + 行定位 5B。
std::string EncodeLeafKeyColumns(const std::vector<Value>& vals, page_id_t page_id,
                                 uint8_t slot_id);

// 解码叶子键的列值部分（按给定列类型解释，单列便捷接口）。
bool DecodeLeafKeyColumn(const std::string& key, ValueType type, Value* out);

// 按类型序列**逐列推进**解码叶子键的列值部分。列数 ≥ 2 时先读 NULL 位图。
// 要求列值部分被完整消费（防「两列错位仍合法」的歧义），否则返回 false。
bool DecodeLeafKeyColumns(const std::string& key, const std::vector<ValueType>& types,
                          std::vector<Value>* out);

// 解码叶子键的行定位部分。
bool DecodeLeafKeyRid(const std::string& key, page_id_t* page_id, uint8_t* slot_id);

// 去掉叶子键尾部的行定位，得到「列值键」（内部节点分隔键形态）。
// 行定位恒在键尾 5 字节 → 复合键去尾后仍是完整元组编码，
// CompareIndexKey 的前缀比较继续成立。
std::string StripLeafRowId(const std::string& leaf_key);

// 比较两个键。b_is_prefix = true 表示 b 是「只含列值部分」的内部键，
// 与 a（列值 + 行定位）的对应前缀比较。
int CompareIndexKey(const std::string& a, const std::string& b, bool b_is_prefix);

// 单列编码的最大字节数（按最坏情况估算：VARCHAR 转义后最长 2n+2）。
// 建索引时用 Σ(各列) + 位图 + 行定位做键长上限校验。
size_t MaxEncodedColumnLen(ValueType type, uint16_t max_len);

// 复合键的 NULL 位图字节数（列数 < 2 时为 0 —— 单列不加位图）。
size_t NullBitmapBytes(size_t column_count);

// 键的尾部行定位长度（供空间估算）
constexpr size_t kIndexLeafRidBytes = 5;

}  // namespace cella::storage
