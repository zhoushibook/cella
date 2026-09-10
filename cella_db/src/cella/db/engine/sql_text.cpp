#include "cella/db/engine/sql_text.h"

#include <cctype>

namespace cella::db {
namespace {

bool IsSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

bool OnlyWhitespaceOrComments(const std::string& s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const char c = s[i];
    if (IsSpace(c)) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < n && s[i + 1] == '-') {
      while (i < n && s[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < n) ? i + 2 : n;
      continue;
    }
    return false;
  }
  return true;
}

// 跳过空白与注释，返回首个有效字符的下标
size_t SkipTrivia(const std::string& s, size_t i) {
  const size_t n = s.size();
  while (i < n) {
    if (IsSpace(s[i])) {
      ++i;
      continue;
    }
    if (s[i] == '-' && i + 1 < n && s[i + 1] == '-') {
      while (i < n && s[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < n) ? i + 2 : n;
      continue;
    }
    break;
  }
  return i;
}

// 取首个标识符词（先跳过注释/空白，"" 表示没有词）："BEGIN;" → "BEGIN"
std::string FirstWord(const std::string& s) {
  std::string word;
  const size_t n = s.size();
  for (size_t i = SkipTrivia(s, 0); i < n; ++i) {
    const bool ident = std::isalnum(static_cast<unsigned char>(s[i])) != 0 || s[i] == '_';
    if (!ident) {
      break;  // 遇到 ';' '(' 等非标识符字符即结束
    }
    word += static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
  }
  return word;
}

}  // namespace

std::string TrimUpper(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && IsSpace(s[b])) {
    ++b;
  }
  while (e > b && IsSpace(s[e - 1])) {
    --e;
  }
  std::string out;
  out.reserve(e - b);
  for (size_t i = b; i < e; ++i) {
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
  }
  return out;
}

std::vector<SqlStatement> SplitSqlStatements(const std::string& sql) {
  std::vector<SqlStatement> out;
  const size_t n = sql.size();
  std::string cur;
  int line = 1;
  int col = 1;
  int start_line = 0;
  int start_col = 0;
  bool started = false;
  bool in_block_comment = false;

  auto note_start = [&](char c) {
    if (!started && !IsSpace(c)) {
      started = true;
      start_line = line;
      start_col = col;
    }
  };

  for (size_t i = 0; i < n;) {
    const char c = sql[i];

    if (in_block_comment) {
      cur += c;
      ++i;
      ++col;
      if (c == '*' && i < n && sql[i] == '/') {
        cur += '/';
        ++i;
        ++col;
        in_block_comment = false;
      }
      continue;
    }

    // 行注释：整段原样复制到行尾
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        cur += sql[i];
        ++i;
        ++col;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      cur += "/*";
      i += 2;
      col += 2;
      in_block_comment = true;
      continue;
    }

    // 字符串字面量：内部 '' 为转义；分号/注释符在字符串内不生效
    if (c == '\'') {
      note_start(c);
      cur += c;
      ++i;
      ++col;
      while (i < n) {
        const char d = sql[i];
        cur += d;
        ++i;
        if (d == '\n') {
          ++line;
          col = 1;
          continue;
        }
        ++col;
        if (d == '\'') {
          if (i < n && sql[i] == '\'') {  // '' 转义，继续留在字符串内
            cur += sql[i];
            ++i;
            ++col;
            continue;
          }
          break;
        }
      }
      continue;
    }

    if (c == ';') {
      cur += c;
      ++i;
      ++col;
      SqlStatement st;
      st.text = cur;
      st.line = started ? start_line : line;
      st.col = started ? start_col : col;
      st.terminated = true;
      out.push_back(std::move(st));
      cur.clear();
      started = false;
      start_line = 0;
      start_col = 0;
      continue;
    }

    if (c == '\n') {
      cur += c;
      ++i;
      ++line;
      col = 1;
      continue;
    }

    note_start(c);
    cur += c;
    ++i;
    ++col;
  }

  if (!OnlyWhitespaceOrComments(cur)) {
    SqlStatement st;
    st.text = cur;
    st.line = started ? start_line : line;
    st.col = started ? start_col : col;
    st.terminated = false;
    out.push_back(std::move(st));
  }
  return out;
}

bool IsTxnControl(const std::string& stmt_text, std::string* keyword) {
  const std::string word = FirstWord(stmt_text);
  const bool hit = (word == "BEGIN" || word == "COMMIT" || word == "ROLLBACK" || word == "END" ||
                    word == "START");
  if (hit && keyword != nullptr) {
    *keyword = (word == "START") ? std::string("BEGIN") : word;
  }
  return hit;
}

}  // namespace cella::db
