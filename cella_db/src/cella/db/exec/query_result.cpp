#include "cella/db/exec/query_result.h"

#include <algorithm>
#include <sstream>

#include "cella/db/common/value_bridge.h"

namespace cella::db {
namespace {

// 单码点显示宽度：CJK / 全角区间记 2，其余记 1
size_t CodepointWidth(uint32_t cp) {
  if (cp < 0x80) {
    return 1;
  }
  // CJK 统一表意文字、全角标点、日文假名、韩文音节等常见宽字符
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD)) {
    return 2;
  }
  return 1;
}

std::string PadRight(const std::string& s, size_t width) {
  const size_t w = DisplayWidth(s);
  if (w >= width) {
    return s;
  }
  return s + std::string(width - w, ' ');
}

// 生成 n 个显示宽度的横线
std::string Dashes(size_t width) { return std::string(width, '-'); }

}  // namespace

size_t DisplayWidth(const std::string& utf8) {
  size_t width = 0;
  size_t i = 0;
  const size_t n = utf8.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0;
    size_t adv = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      cp = static_cast<uint32_t>(c & 0x1F) << 6;
      cp |= static_cast<unsigned char>(utf8[i + 1]) & 0x3F;
      adv = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      cp = static_cast<uint32_t>(c & 0x0F) << 12;
      cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6;
      cp |= static_cast<unsigned char>(utf8[i + 2]) & 0x3F;
      adv = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      cp = static_cast<uint32_t>(c & 0x07) << 18;
      cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12;
      cp |= (static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6;
      cp |= static_cast<unsigned char>(utf8[i + 3]) & 0x3F;
      adv = 4;
    } else {
      cp = c;  // 非法字节：按 1 宽 1 字节处理，保证不崩
    }
    width += CodepointWidth(cp);
    i += adv;
  }
  return width;
}

std::string QueryResult::ToText() const {
  if (columns.empty()) {
    return Summary();
  }
  const size_t ncol = columns.size();
  std::vector<std::string> header;
  std::vector<size_t> width;
  header.reserve(ncol);
  width.reserve(ncol);
  for (const auto& c : columns) {
    header.push_back(c.name);
    width.push_back(DisplayWidth(c.name));
  }
  // 渲染全部单元格（同时算列宽）
  std::vector<std::vector<std::string>> cells;
  cells.reserve(rows.size());
  for (const auto& row : rows) {
    std::vector<std::string> line;
    line.reserve(ncol);
    for (size_t i = 0; i < ncol; ++i) {
      const std::string text = (i < row.size()) ? RenderValue(row[i]) : "NULL";
      width[i] = std::max(width[i], DisplayWidth(text));
      line.push_back(text);
    }
    cells.push_back(std::move(line));
  }

  std::ostringstream os;
  for (size_t i = 0; i < ncol; ++i) {
    if (i != 0) {
      os << " | ";
    }
    os << PadRight(header[i], width[i]);
  }
  os << "\n";
  for (size_t i = 0; i < ncol; ++i) {
    if (i != 0) {
      os << "-+-";
    }
    os << Dashes(width[i]);
  }
  os << "\n";
  for (const auto& line : cells) {
    for (size_t i = 0; i < ncol; ++i) {
      if (i != 0) {
        os << " | ";
      }
      os << PadRight(line[i], width[i]);
    }
    os << "\n";
  }
  os << "(" << rows.size() << " 行)";
  return os.str();
}

std::string QueryResult::Summary() const {
  if (!tag.empty()) {
    return tag;
  }
  return "(" + std::to_string(affected) + " 行受影响)";
}

}  // namespace cella::db
