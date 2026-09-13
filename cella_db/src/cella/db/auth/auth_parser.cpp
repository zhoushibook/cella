#include "cella/db/auth/auth_parser.h"

#include <cctype>
#include <utility>

namespace cella::db {
namespace {

std::string Upper(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

bool IsSpaceChar(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

bool IsWordChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '_';
}

// ── 极简 tokenizer ──────────────────────────────────────────
struct Token {
  enum class Kind { kWord, kString, kPunct, kEnd };
  Kind kind = Kind::kEnd;
  std::string text;  // Word → 原文；String → 已解转义内容；Punct → 单字符
};

// 词法不完整（字符串未闭合）时 *ok = false，调用方应交回编译器报错
std::vector<Token> Tokenize(const std::string& s, bool* ok) {
  std::vector<Token> out;
  *ok = true;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (IsSpaceChar(c)) {
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
    if (IsWordChar(c)) {
      const size_t b = i;
      while (i < n && IsWordChar(s[i])) {
        ++i;
      }
      Token t;
      t.kind = Token::Kind::kWord;
      t.text = s.substr(b, i - b);
      out.push_back(std::move(t));
      continue;
    }
    if (c == '\'') {
      ++i;
      std::string val;
      bool closed = false;
      while (i < n) {
        if (s[i] == '\'') {
          if (i + 1 < n && s[i + 1] == '\'') {  // '' 转义为一个单引号
            val += '\'';
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        val += s[i];
        ++i;
      }
      if (!closed) {
        *ok = false;
        return out;
      }
      Token t;
      t.kind = Token::Kind::kString;
      t.text = std::move(val);
      out.push_back(std::move(t));
      continue;
    }
    Token t;
    t.kind = Token::Kind::kPunct;
    t.text = std::string(1, c);
    out.push_back(std::move(t));
    ++i;
  }
  Token e;
  e.kind = Token::Kind::kEnd;
  out.push_back(std::move(e));
  return out;
}

// ── 游标 ────────────────────────────────────────────────────
struct Cursor {
  const std::vector<Token>* toks = nullptr;
  size_t i = 0;

  const Token& Peek(size_t k = 0) const { return (*toks)[i + k]; }
  // 定位到第 k 个 token（越界则停在末尾的 End token，保证 Peek 安全）
  void Seek(size_t k) { i = (k < toks->size()) ? k : (toks->size() - 1); }
  bool AtEnd() const { return Peek().kind == Token::Kind::kEnd; }
  // 当前词（大写）；非词返回空
  std::string Word() const {
    return Peek().kind == Token::Kind::kWord ? Upper(Peek().text) : std::string();
  }
  std::string WordAt(size_t k) const {
    return Peek(k).kind == Token::Kind::kWord ? Upper(Peek(k).text) : std::string();
  }
  bool TakeWord(std::string* raw) {
    if (Peek().kind != Token::Kind::kWord) {
      return false;
    }
    if (raw != nullptr) {
      *raw = Peek().text;
    }
    ++i;
    return true;
  }
  bool TakeKeyword(const char* kw) {
    if (Word() != kw) {
      return false;
    }
    ++i;
    return true;
  }
  bool TakePunct(char c) {
    if (Peek().kind != Token::Kind::kPunct || Peek().text.size() != 1 || Peek().text[0] != c) {
      return false;
    }
    ++i;
    return true;
  }
  bool TakeString(std::string* out) {
    if (Peek().kind != Token::Kind::kString) {
      return false;
    }
    if (out != nullptr) {
      *out = Peek().text;
    }
    ++i;
    return true;
  }
  // 剩余只允许一个收尾分号
  bool TrailingOk() const {
    if (Peek().kind == Token::Kind::kEnd) {
      return true;
    }
    if (Peek().kind == Token::Kind::kPunct && Peek().text == ";") {
      return Peek(1).kind == Token::Kind::kEnd;
    }
    return false;
  }
};

struct ParseCtx {
  Cursor cur;
  std::string* err = nullptr;

  AuthParse Fail(const std::string& msg) {
    if (err != nullptr) {
      *err = msg;
    }
    return AuthParse::kSyntaxError;
  }
};

// 作用域：`*.*` / `db.*` / `db.table` / `table`（无库部分时 scope_db 留空，由执行方填当前库）
bool ParseScope(Cursor* cur, std::string* db, std::string* table) {
  db->clear();
  table->clear();
  // 第一段：'*' 或词
  bool first_star = false;
  std::string first;
  if (!cur->TakeWord(&first)) {
    if (cur->TakePunct('*')) {
      first_star = true;
    } else {
      return false;
    }
  }
  if (!cur->TakePunct('.')) {
    if (first_star) {
      return false;  // 单有一个 '*' 不构成作用域
    }
    *table = first;  // `ON table`：当前库的单表
    return true;
  }
  // 第二段：'*' 或词
  bool second_star = false;
  std::string second;
  if (!cur->TakeWord(&second)) {
    if (cur->TakePunct('*')) {
      second_star = true;
    } else {
      return false;
    }
  }
  if (first_star) {
    if (!second_star) {
      return false;  // `*.table` 无意义
    }
    *db = "*";
    *table = "*";
    return true;
  }
  *db = first;
  *table = second_star ? "*" : second;
  return true;
}

// 逗号分隔的词列表
bool ParseWordList(Cursor* cur, std::vector<std::string>* out) {
  out->clear();
  std::string w;
  if (!cur->TakeWord(&w)) {
    return false;
  }
  out->push_back(w);
  while (cur->TakePunct(',')) {
    if (!cur->TakeWord(&w)) {
      return false;
    }
    out->push_back(w);
  }
  return true;
}

}  // namespace

std::string FormatScope(const std::string& scope_db, const std::string& scope_table) {
  const std::string db = scope_db.empty() ? "*" : scope_db;
  const std::string tbl = scope_table.empty() ? "*" : scope_table;
  return db + "." + tbl;
}

AuthParse ParseUserCommand(const std::string& stmt_text, UserCommand* out, std::string* err) {
  bool lex_ok = true;
  const std::vector<Token> toks = Tokenize(stmt_text, &lex_ok);
  if (!lex_ok) {
    return AuthParse::kNotAuth;  // 词法都不完整 → 交给编译器报错
  }
  ParseCtx ctx;
  ctx.cur.toks = &toks;
  ctx.err = err;

  const std::string w1 = ctx.cur.WordAt(0);
  const std::string w2 = ctx.cur.WordAt(1);

  if (w1 == "CREATE" || w1 == "DROP") {
    if (w2 != "USER") {
      return AuthParse::kNotAuth;
    }
    ctx.cur.Seek(2);  // 跳过 CREATE/DROP USER（前两个 token）
    UserCommand c;
    if (w1 == "CREATE") {
      c.kind = UserCommand::Kind::kCreateUser;
      if (!ctx.cur.TakeWord(&c.name)) {
        return ctx.Fail("CREATE USER 缺少用户名");
      }
      if (ctx.cur.TakeKeyword("IDENTIFIED")) {
        if (!ctx.cur.TakeKeyword("BY")) {
          return ctx.Fail("IDENTIFIED 之后期望 BY");
        }
        if (!ctx.cur.TakeString(&c.password)) {
          return ctx.Fail("BY 之后期望单引号字符串口令");
        }
        c.has_password = true;
      }
    } else {
      c.kind = UserCommand::Kind::kDropUser;
      if (!ctx.cur.TakeWord(&c.name)) {
        return ctx.Fail("DROP USER 缺少用户名");
      }
    }
    if (!ctx.cur.TrailingOk()) {
      return ctx.Fail("语句尾部有多余内容");
    }
    *out = std::move(c);
    return AuthParse::kOk;
  }

  if (w1 == "SET") {
    if (w2 != "PASSWORD") {
      return AuthParse::kNotAuth;
    }
    ctx.cur.Seek(2);
    UserCommand c;
    c.kind = UserCommand::Kind::kSetPassword;
    if (ctx.cur.TakeKeyword("FOR")) {
      if (!ctx.cur.TakeWord(&c.name)) {
        return ctx.Fail("SET PASSWORD FOR 之后期望用户名");
      }
    }
    if (!ctx.cur.TakePunct('=')) {
      return ctx.Fail("SET PASSWORD 之后期望 '='");
    }
    if (!ctx.cur.TakeString(&c.password)) {
      return ctx.Fail("期望单引号字符串口令");
    }
    c.has_password = true;
    if (!ctx.cur.TrailingOk()) {
      return ctx.Fail("语句尾部有多余内容");
    }
    *out = std::move(c);
    return AuthParse::kOk;
  }

  if (w1 == "SHOW" && w2 == "USERS") {
    ctx.cur.Seek(2);
    if (!ctx.cur.TrailingOk()) {
      return ctx.Fail("SHOW USERS 不接受参数");
    }
    UserCommand c;
    c.kind = UserCommand::Kind::kShowUsers;
    *out = std::move(c);
    return AuthParse::kOk;
  }

  return AuthParse::kNotAuth;
}

AuthParse ParseGrantCommand(const std::string& stmt_text, GrantCommand* out, std::string* err) {
  bool lex_ok = true;
  const std::vector<Token> toks = Tokenize(stmt_text, &lex_ok);
  if (!lex_ok) {
    return AuthParse::kNotAuth;
  }
  ParseCtx ctx;
  ctx.cur.toks = &toks;
  ctx.err = err;

  const std::string w1 = ctx.cur.WordAt(0);

  if (w1 == "SHOW" && ctx.cur.WordAt(1) == "GRANTS") {
    ctx.cur.Seek(2);
    GrantCommand c;
    c.kind = GrantCommand::Kind::kShowGrants;
    if (ctx.cur.TakeKeyword("FOR")) {
      if (!ctx.cur.TakeWord(&c.name)) {
        return ctx.Fail("SHOW GRANTS FOR 之后期望用户名");
      }
    }
    if (!ctx.cur.TrailingOk()) {
      return ctx.Fail("SHOW GRANTS 语句尾部有多余内容");
    }
    *out = std::move(c);
    return AuthParse::kOk;
  }

  if (w1 != "GRANT" && w1 != "REVOKE") {
    return AuthParse::kNotAuth;
  }
  ctx.cur.Seek(1);
  GrantCommand c;
  c.kind = (w1 == "GRANT") ? GrantCommand::Kind::kGrant : GrantCommand::Kind::kRevoke;

  if (!ParseWordList(&ctx.cur, &c.privs)) {
    return ctx.Fail(std::string(w1 == "GRANT" ? "GRANT" : "REVOKE") + " 缺少权限名（如 get / insert / all）");
  }
  for (std::string& p : c.privs) {
    p = Upper(p);
  }
  if (!ctx.cur.TakeKeyword("ON")) {
    return ctx.Fail("期望关键字 ON");
  }
  if (!ParseScope(&ctx.cur, &c.scope_db, &c.scope_table)) {
    return ctx.Fail("ON 之后期望作用域（*.* / 库名.* / 库名.表名 / 表名）");
  }
  const char* link = (c.kind == GrantCommand::Kind::kGrant) ? "TO" : "FROM";
  if (!ctx.cur.TakeKeyword(link)) {
    return ctx.Fail(std::string("期望关键字 ") + link);
  }
  if (!ParseWordList(&ctx.cur, &c.users)) {
    return ctx.Fail(std::string(link) + " 之后期望用户名列表");
  }
  if (!ctx.cur.TrailingOk()) {
    return ctx.Fail("语句尾部有多余内容");
  }
  *out = std::move(c);
  return AuthParse::kOk;
}

}  // namespace cella::db
