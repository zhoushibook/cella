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
      // 剥掉前导空白/注释：text 必须从首个有效字符开始，与 st.line/col（也是首个有效
      // 字符的绝对位置）同一基准。否则编译器收到的文本首行是空的，诊断行号会整体偏移。
      st.text = cur.substr(SkipTrivia(cur, 0));
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
    st.text = cur.substr(SkipTrivia(cur, 0));  // 同上：与 st.line/col 对齐基准
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

namespace {

// 从下标 i 取一个标识符词（原文大小写）。成功返回 true 并把 i 推进到词后。
bool TakeWord(const std::string& s, size_t* i, std::string* word) {
  const size_t n = s.size();
  size_t j = SkipTrivia(s, *i);
  size_t b = j;
  while (j < n) {
    const char c = s[j];
    const bool ident = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    if (!ident) {
      break;
    }
    ++j;
  }
  if (j == b) {
    return false;
  }
  *word = s.substr(b, j - b);
  *i = j;
  return true;
}

// 剩余部分只允许空白/注释/结尾分号（语句文本自带分号）
bool RestIsTrivia(const std::string& s, size_t i) {
  const size_t n = s.size();
  std::string tail;
  while (i < n) {
    if (s[i] == ';') {
      ++i;
      continue;
    }
    tail += s[i];
    ++i;
  }
  return OnlyWhitespaceOrComments(tail);
}

std::string Upper(const std::string& w) { return TrimUpper(w); }

}  // namespace

bool IsDatabaseControl(const std::string& stmt_text, std::string* kind, std::string* arg) {
  size_t i = 0;
  std::string w1;
  if (!TakeWord(stmt_text, &i, &w1)) {
    return false;
  }
  const size_t after_w1 = i;  // 第一个词之后的位置（EXPLAIN 取尾串要用）
  const std::string u1 = Upper(w1);
  std::string w2;
  const bool has_w2 = TakeWord(stmt_text, &i, &w2);
  const std::string u2 = has_w2 ? Upper(w2) : std::string();

  if (u1 == "USE") {
    if (!has_w2 || !RestIsTrivia(stmt_text, i)) {
      return false;  // "USE" 缺名/带垃圾 → 交给编译器报标准语法错
    }
    if (kind != nullptr) *kind = "USE";
    if (arg != nullptr) *arg = w2;
    return true;
  }
  if ((u1 == "CREATE" || u1 == "DROP") && u2 == "DATABASE") {
    std::string w3;
    if (!TakeWord(stmt_text, &i, &w3) || !RestIsTrivia(stmt_text, i)) {
      return false;
    }
    if (kind != nullptr) *kind = u1 + " DATABASE";
    if (arg != nullptr) *arg = w3;
    return true;
  }
  if (u1 == "SHOW" && u2 == "DATABASES") {
    if (!RestIsTrivia(stmt_text, i)) {
      return false;
    }
    if (kind != nullptr) *kind = "SHOW DATABASES";
    if (arg != nullptr) arg->clear();
    return true;
  }
  // SHOW INDEXES            → 列出所有索引
  // SHOW INDEXES IN <table> → 只看某张表的索引
  if (u1 == "SHOW" && u2 == "INDEXES") {
    std::string w3;
    if (!TakeWord(stmt_text, &i, &w3)) {
      // 无附加词：整体列出
      if (kind != nullptr) *kind = "SHOW INDEXES";
      if (arg != nullptr) arg->clear();
      return true;
    }
    if (Upper(w3) != "IN" || !TakeWord(stmt_text, &i, &w3) || !RestIsTrivia(stmt_text, i)) {
      return false;
    }
    if (kind != nullptr) *kind = "SHOW INDEXES";
    if (arg != nullptr) *arg = w3;
    return true;
  }
  // EXPLAIN <任意语句>：arg 返回 EXPLAIN 之后的**原始尾串**（保留原拼写，
  // 由会话层送进编译器），用于展示访问路径选择结果。空尾串 → 交给编译器报错。
  if (u1 == "EXPLAIN") {
    // 注意：i 此时已越过第二个词（上面的 TakeWord(w2) 有无条件推进），
    // 所以尾串必须从 after_w1 重新开始，否则会把 "get" 吃掉。
    size_t b = after_w1;
    size_t e = stmt_text.size();
    while (b < e && IsSpace(stmt_text[b])) {
      ++b;
    }
    while (e > b && IsSpace(stmt_text[e - 1])) {
      --e;
    }
    const std::string rest = stmt_text.substr(b, e - b);
    if (rest.empty()) {
      return false;
    }
    if (kind != nullptr) *kind = "EXPLAIN";
    if (arg != nullptr) *arg = rest;
    return true;
  }
  return false;
}

}  // namespace cella::db
