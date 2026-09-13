// main.cpp —— cella_db 统一 CLI：交互式 REPL + 脚本执行 + 诊断元命令。
//
// 用法：
//   cella_db [选项] [SQL 文件 ...]
//
// 不带文件时进入 REPL；带文件时按顺序执行脚本（每条语句一次汇报），
// 退出码 0 = 全部成功，1 = 存在编译或执行失败，2 = 用法错误。
//
// REPL 元命令（以 \ 开头，不进入编译器）：
//   \q  退出          \?  帮助          \d [表] 表清单/表结构
//   \plan <SQL>      只编译并打印计划（优化前后对比）
//   \stats   缓冲池统计         \locks  锁表        \waitfor  等待图
//   \txn     事务表             \timing on|off      \echo on|off
//   \help    同 \?
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cella/db/engine/db_engine.h"
#include "cella/db/engine/sql_text.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
#endif

namespace {

using cella::db::DbEngine;
using cella::db::EngineConfig;
using cella::db::ScriptReport;
using cella::db::Session;
using cella::db::StatementOutcome;

struct Options {
  EngineConfig engine;
  std::vector<std::string> files;
  std::string user;      // --user（--auth 时的登录名）
  std::string password;  // --password（注意：命令行参数会出现在进程列表里）
  bool verbose = false;
  bool show_plan = false;
  bool echo = false;
  bool stats_at_exit = false;
  bool timing = false;
  bool help = false;
};

void PrintUsage(std::ostream& os) {
  os << "cella 数据库系统（编译器 + 存储引擎 + 数据库层整合版）\n"
        "\n"
        "用法: cella_db [选项] [SQL 文件 ...]\n"
        "\n"
        "选项:\n"
        "  --data DIR          数据目录（默认 ./cella_data）\n"
        "  -f, --file FILE     执行 SQL 脚本文件（可重复；也可直接作为位置参数）\n"
        "  --db NAME           启动数据库（库 = <data_dir>/NAME.db，默认 main）\n"
        "  --page-size N       页大小字节数（2 的幂，默认 4096）\n"
        "  --pool N            缓冲池帧数（默认 64）\n"
        "  --replacer NAME     替换策略 LRU|FIFO|CLOCK（默认 LRU）\n"
        "  --log LEVEL         日志级别 debug|info|warn|error|off（默认 info）\n"
        "  --log-console       日志同时输出到控制台\n"
        "  --lock-timeout MS   锁等待超时毫秒（默认 5000）\n"
        "  --no-journal        不写事务审计日志\n"
        "  --checkpoint-on-commit  每次提交都把数据文件落盘（更安全，但更慢）\n"
        "  --auth              启用访问控制（需登录；首次自动创建管理员 root，空口令）\n"
        "  -u, --user NAME     登录用户名（配合 --auth）\n"
        "  -p, --password PW   登录口令（配合 --auth；交互模式下省略则提示输入）\n"
        "  -v, --verbose       打印计划、耗时、算子调用次数\n"
        "  --show-plan         只编译并打印执行计划，不真正执行\n"
        "  --echo              回显每条语句\n"
        "  --timing            打印每条语句耗时\n"
        "  --stats             退出前打印缓冲池统计\n"
        "  -h, --help          显示本帮助\n"
        "\n"
        "REPL 元命令: \\? \\q \\d [表] \\l \\plan <SQL> \\stats \\locks \\waitfor \\txn \\timing \\echo\n"
        "              \\whoami \\users \\passwd <新口令>\n";
}

bool ParseLevel(const std::string& s, cella::storage::LogLevel* out) {
  if (s == "debug") {
    *out = cella::storage::LogLevel::kDebug;
    return true;
  }
  if (s == "info") {
    *out = cella::storage::LogLevel::kInfo;
    return true;
  }
  if (s == "warn") {
    *out = cella::storage::LogLevel::kWarn;
    return true;
  }
  if (s == "error") {
    *out = cella::storage::LogLevel::kError;
    return true;
  }
  return false;
}

bool ParseArgs(int argc, char** argv, Options* opt, std::string* err) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need_value = [&](std::string* dst) -> bool {
      if (i + 1 >= argc) {
        *err = "选项缺少参数: " + a;
        return false;
      }
      *dst = argv[++i];
      return true;
    };

    if (a == "-h" || a == "--help") {
      opt->help = true;
    } else if (a == "--data") {
      if (!need_value(&opt->engine.data_dir)) return false;
    } else if (a == "-f" || a == "--file") {
      std::string v;
      if (!need_value(&v)) return false;
      opt->files.push_back(v);
    } else if (a == "--db") {
      if (!need_value(&opt->engine.db_file)) return false;
    } else if (a == "--page-size") {
      std::string v;
      if (!need_value(&v)) return false;
      opt->engine.page_size = static_cast<uint32_t>(std::stoul(v));
    } else if (a == "--pool") {
      std::string v;
      if (!need_value(&v)) return false;
      opt->engine.pool_size = static_cast<size_t>(std::stoul(v));
    } else if (a == "--replacer") {
      if (!need_value(&opt->engine.replacer)) return false;
    } else if (a == "--log") {
      std::string v;
      if (!need_value(&v)) return false;
      if (v == "off") {
        opt->engine.enable_log = false;
      } else if (!ParseLevel(v, &opt->engine.log_level)) {
        *err = "未知日志级别: " + v;
        return false;
      }
    } else if (a == "--log-console") {
      opt->engine.log_to_console = true;
    } else if (a == "--lock-timeout") {
      std::string v;
      if (!need_value(&v)) return false;
      opt->engine.lock_timeout = std::chrono::milliseconds(std::stol(v));
    } else if (a == "--no-journal") {
      opt->engine.enable_journal = false;
    } else if (a == "-v" || a == "--verbose") {
      opt->verbose = true;
    } else if (a == "--show-plan") {
      opt->show_plan = true;
    } else if (a == "--echo") {
      opt->echo = true;
    } else if (a == "--timing") {
      opt->timing = true;
    } else if (a == "--stats") {
      opt->stats_at_exit = true;
    } else if (a == "--checkpoint-on-commit") {
      opt->engine.checkpoint_on_commit = true;
    } else if (a == "--auth") {
      opt->engine.enable_auth = true;
    } else if (a == "-u" || a == "--user") {
      if (!need_value(&opt->user)) return false;
    } else if (a == "-p" || a == "--password") {
      if (!need_value(&opt->password)) return false;
    } else if (!a.empty() && a[0] == '-') {
      *err = "未知选项: " + a;
      return false;
    } else {
      opt->files.push_back(a);
    }
  }
  return true;
}

// ── 单条语句结果的渲染 ──────────────────────────────────────
void PrintOutcome(const StatementOutcome& out, bool verbose, bool timing, size_t index) {
  std::cout << "-- 语句 #" << index << " [" << out.kind << "] @" << out.line << ":" << out.col
            << "\n";
  if (!out.notice.empty()) {
    std::cout << "-- " << out.notice << "\n";
  }
  for (const auto& e : out.compile_errors) {
    std::cout << cella::cella_errorText(e) << "\n";
  }
  if (!out.status.ok() && out.compile_errors.empty()) {
    std::cout << out.status.ToString() << "\n";
  } else if (out.status.ok() && out.executed) {
    if (out.result.IsQuery()) {
      std::cout << out.result.ToText() << "\n";
    } else if (!out.result.tag.empty()) {
      std::cout << out.result.tag << "\n";
    }
  }
  if (verbose && !out.plan_text.empty()) {
    std::cout << "   计划(优化后):\n";
    std::istringstream in(out.plan_text);
    std::string line;
    while (std::getline(in, line)) {
      std::cout << "     " << line << "\n";
    }
    std::cout << "   算子调用 " << out.operator_calls << " 次 / 事务 txn=" << out.txn_id;
    if (out.auto_committed) {
      std::cout << "（自动提交）";
    }
    if (out.rolled_back_here) {
      std::cout << "（语句级回滚）";
    }
    std::cout << "\n";
  }
  if (timing) {
    std::cout << "   Time: " << cella::db::FormatMillis(out.elapsed_ms) << "\n";
  }
}

// ── 执行一段 SQL 并渲染 ─────────────────────────────────────
// 返回 >0 表示语句条数，负数表示失败条数（调用方用于统计）
void RunSql(Session& session, const std::string& sql, const Options& opt, size_t* ok,
            size_t* failed) {
  ScriptReport report;
  if (opt.show_plan) {
    (void)session.CompileOnly(sql, &report);
  } else {
    (void)session.Execute(sql, &report);
  }
  size_t idx = 0;
  for (const auto& s : report.statements) {
    ++idx;
    PrintOutcome(s, opt.verbose || opt.show_plan, opt.timing, idx);
    if (s.status.ok() && s.compile_errors.empty()) {
      *ok += 1;
    } else {
      *failed += 1;
    }
  }
}

// SQL 字符串字面量转义（单引号翻倍）
std::string EscapeSqlString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  return out;
}

void PrintReplHelp() {
  std::cout << "可用元命令（以 \\ 开头）：\n"
               "  \\?                显示本帮助\n"
               "  \\q                退出\n"
               "  \\d                列出所有表\n"
               "  \\d <表名>         显示表结构\n"
               "  \\l                列出所有数据库\n"
               "  \\plan <SQL>       只编译并打印计划（优化前/后对比）\n"
               "  \\stats            缓冲池统计（命中率/淘汰/钉住）\n"
               "  \\locks            当前锁表\n"
               "  \\waitfor          等待图（死锁检测依据）\n"
               "  \\txn              事务表\n"
               "  \\checkpoint       立即把数据文件落盘（存盘点）\n"
               "  \\whoami           显示当前登录用户与角色\n"
               "  \\users            列出所有用户（等价 SHOW USERS;）\n"
               "  \\passwd <新口令>  修改自己的口令（等价 SET PASSWORD = '...';）\n"
               "  \\timing on|off    打印每条语句耗时\n"
               "  \\echo on|off      回显每条语句\n"
               "SQL 语句以分号 ';' 结束（可跨多行）；事务用 BEGIN; / COMMIT; / ROLLBACK;\n"
               "多库：CREATE DATABASE 名; / DROP DATABASE 名; / USE 名; / SHOW DATABASES;\n"
               "访问控制：CREATE USER 名 IDENTIFIED BY '口令'; / DROP USER 名;\n"
               "          GRANT get, insert ON 库.表 TO 名; / SHOW GRANTS;\n";
}

int RunRepl(DbEngine& engine, Options& opt) {
  Session& session = engine.default_session();
  std::cout << "cella_db 已就绪 —— 当前库 " << engine.current_db() << "，"
            << session.StatusLine() << "\n"
            << "数据目录: " << engine.config().data_dir << " / 页大小 " << engine.config().page_size
            << " / 缓冲池 " << engine.config().pool_size << " 帧 / 替换策略 "
            << engine.config().replacer << "\n"
            << "输入 SQL（分号结束），或 \\? 查看元命令，\\q 退出。\n";

  std::string buffer;
  size_t ok = 0;
  size_t failed = 0;

  while (true) {
    std::string prompt = "cella(" + engine.current_db() + ")";
    if (!session.user().empty()) {
      prompt += " " + session.user();
    }
    std::cout << (buffer.empty() ? (prompt + "> ") : "    ...> ") << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }

    // 元命令：仅在没有待续语句时识别
    if (buffer.empty() && !line.empty() && line[0] == '\\') {
      const std::string trimmed = cella::db::TrimUpper(line);
      if (trimmed == "\\Q") {
        break;
      } else if (trimmed == "\\?" || trimmed == "\\H" || trimmed == "\\HELP") {
        PrintReplHelp();
      } else if (trimmed == "\\D" || trimmed == "\\DT") {
        std::cout << engine.catalog().Describe();
      } else if (trimmed.rfind("\\D ", 0) == 0) {
        std::string arg = line.substr(3);
        while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) {
          arg.erase(arg.begin());
        }
        std::cout << engine.catalog().DescribeTable(arg);
      } else if (trimmed == "\\STATS") {
        std::cout << engine.StatsText();
      } else if (trimmed == "\\LOCKS") {
        std::cout << engine.LockText();
      } else if (trimmed == "\\WAITFOR") {
        std::cout << engine.WaitForGraphText();
      } else if (trimmed == "\\TXN") {
        std::cout << engine.TxnText();
      } else if (trimmed == "\\CHECKPOINT") {
        const cella::db::DbStatus cp = engine.Checkpoint();
        std::cout << (cp.ok() ? "数据文件已落盘（存盘点完成）。" : cp.ToString()) << "\n";
      } else if (trimmed == "\\L" || trimmed == "\\LIST") {
        cella::db::QueryResult dbs;
        const cella::db::DbStatus st = engine.ShowDatabases(&dbs);
        if (st.ok()) {
          std::cout << "当前库: " << engine.current_db() << "\n" << dbs.ToText();
        } else {
          std::cout << st.ToString() << "\n";
        }
      } else if (trimmed == "\\WHOAMI" || trimmed == "\\WHO") {
        if (session.user().empty()) {
          std::cout << "未启用访问控制（所有操作按管理员处理）。\n";
        } else {
          std::cout << "当前用户: " << session.user()
                    << (session.is_admin() ? "（管理员）" : "（普通用户）") << " / 当前库 "
                    << engine.current_db() << "\n";
        }
      } else if (trimmed == "\\USERS" || trimmed == "\\U") {
        RunSql(session, "SHOW USERS;", opt, &ok, &failed);
      } else if (trimmed.rfind("\\PASSWD", 0) == 0) {
        std::string pwd = line.substr(7);
        while (!pwd.empty() && (pwd.front() == ' ' || pwd.front() == '\t')) {
          pwd.erase(pwd.begin());
        }
        if (pwd.empty()) {
          std::cout << "用法: \\passwd <新口令>\n";
        } else {
          RunSql(session, "SET PASSWORD = '" + EscapeSqlString(pwd) + "';", opt, &ok, &failed);
        }
      } else if (trimmed.rfind("\\TIMING", 0) == 0) {
        const bool on = (trimmed.find("OFF") == std::string::npos);
        opt.timing = on;
        std::cout << "timing " << (on ? "on" : "off") << "\n";
      } else if (trimmed.rfind("\\ECHO", 0) == 0) {
        const bool on = (trimmed.find("OFF") == std::string::npos);
        opt.echo = on;
        std::cout << "echo " << (on ? "on" : "off") << "\n";
      } else if (trimmed.rfind("\\PLAN", 0) == 0) {
        std::string sql = line.substr(5);
        ScriptReport report;
        (void)session.CompileOnly(sql, &report);
        for (const auto& s : report.statements) {
          for (const auto& e : s.compile_errors) {
            std::cout << cella::cella_errorText(e) << "\n";
          }
          if (!s.status.ok() && s.compile_errors.empty()) {
            std::cout << s.status.ToString() << "\n";
          }
          if (!s.original_plan_text.empty()) {
            std::cout << "== 优化前 ==\n" << s.original_plan_text << "\n== 优化后 ==\n"
                      << s.plan_text << "\n";
          }
        }
      } else {
        std::cout << "未知元命令: " << line << "（\\? 查看帮助）\n";
      }
      continue;
    }

    buffer += line;
    buffer += '\n';
    if (cella::db::TrimUpper(buffer).empty()) {
      buffer.clear();
      continue;
    }

    // 攒够一条完整语句（最后一条以分号结束）才执行
    const auto stmts = cella::db::SplitSqlStatements(buffer);
    if (stmts.empty() || !stmts.back().terminated) {
      continue;
    }
    if (opt.echo) {
      std::cout << buffer;
    }
    RunSql(session, buffer, opt, &ok, &failed);
    buffer.clear();
  }

  if (opt.stats_at_exit) {
    std::cout << engine.StatsText();
  }
  std::cout << "共执行成功 " << ok << " 条 / 失败 " << failed << " 条。\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(65001);  // 控制台按 UTF-8 输出
  SetConsoleCP(65001);        // 控制台按 UTF-8 读入
#endif

  Options opt;
  std::string err;
  if (!ParseArgs(argc, argv, &opt, &err)) {
    std::cerr << err << "\n";
    PrintUsage(std::cerr);
    return 2;
  }
  if (opt.help) {
    PrintUsage(std::cout);
    return 0;
  }

  DbEngine engine;
  const cella::db::DbStatus open_status = engine.Open(opt.engine);
  if (!open_status.ok()) {
    std::cerr << "打开数据库失败: " << open_status.ToString() << "\n";
    return 1;
  }

  // 访问控制：认证启用时必须先登录，否则后续语句一律报 DB-806
  if (engine.auth_enabled()) {
    std::string user = opt.user;
    std::string pass = opt.password;
    if (user.empty()) {
      if (!opt.files.empty()) {
        std::cerr << "认证已启用：非交互模式请用 --user/--password 提供凭据\n";
        engine.Close();
        return 2;
      }
      std::cout << "用户名: " << std::flush;
      if (!std::getline(std::cin, user)) {
        engine.Close();
        return 1;
      }
      std::cout << "口令: " << std::flush;
      if (!std::getline(std::cin, pass)) {
        engine.Close();
        return 1;
      }
    }
    bool is_admin = false;
    const cella::db::DbStatus ast = engine.Authenticate(user, pass, &is_admin);
    if (!ast.ok()) {
      std::cerr << ast.ToString() << "\n";
      engine.Close();
      return 1;
    }
    engine.default_session().SetIdentity(user, is_admin);
    std::cout << "已登录: " << user << (is_admin ? "（管理员）" : "（普通用户）") << "\n";
  }
  if (!engine.auth_bootstrap_note().empty()) {
    std::cout << "\n*** 安全提示 *** " << engine.auth_bootstrap_note() << "\n\n";
  }

  int rc = 0;
  size_t ok = 0;
  size_t failed = 0;

  if (opt.files.empty()) {
    rc = RunRepl(engine, opt);
  } else {
    for (const auto& path : opt.files) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        std::cerr << "无法打开 SQL 文件: " << path << "\n";
        rc = 1;
        continue;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      std::cout << "== 执行脚本 " << path << " ==\n";
      RunSql(engine.default_session(), ss.str(), opt, &ok, &failed);
    }
    if (opt.stats_at_exit) {
      std::cout << engine.StatsText();
    }
    std::cout << "共执行成功 " << ok << " 条 / 失败 " << failed << " 条。\n";
    rc = (failed == 0) ? 0 : 1;
  }

  engine.Close();
  return rc;
}
