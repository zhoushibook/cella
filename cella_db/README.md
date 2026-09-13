# cella_db —— 数据库层与整合层

把 `cella_sql`（编译器前端）与 `cella_storage`（页式存储 + 缓冲池）连成一个能跑的数据库系统。
本模块是**唯一**同时依赖两者的地方：编译器不认识存储，存储不认识 SQL。

职责一览：

| 组件 | 职责 |
| --- | --- |
| `DbEngine` | 门面：生命周期、组件装配（存储/目录/锁/事务/执行器）、诊断接口 |
| `Session` | 会话：逐条语句调度、事务上下文、错误与回滚策略 |
| `Executor` | 计划驱动执行：算子 → 存储调用；内存内做过滤/投影/连接/排序/分页 |
| `ExprEval` | 表达式求值：类型提升、NULL 三值逻辑 |
| `CatalogManager` | 表元数据（列/类型/长度/NOT NULL/首数据页）持久化与查询 |
| `LockManager` | 表级 S/X 锁、条件变量等待、实时等待图死锁检测 |
| `TxnManager` | 事务号、提交/回滚、语句级回滚水位、审计日志 |

---

## 1. 构建与运行

```powershell
# 在仓库根目录
powershell -ExecutionPolicy Bypass -File build.ps1
# 或整体验证（含三层测试与示例）
powershell -ExecutionPolicy Bypass -File run_all.ps1
```

产物：`build/cella_db/cella_db.exe`、`cella_db_tests.exe`、`api_quickstart.exe`、`concurrency_demo.exe`

---

## 2. CLI 用法

```
用法: cella_db [选项] [SQL 文件 ...]

  --data DIR          数据目录（默认 ./cella_data）
  -f, --file FILE     执行 SQL 脚本（可重复；也可直接作为位置参数）
  --db NAME           启动库名（库 = <data_dir>/NAME.db，默认 main）
  --page-size N       页大小字节数（2 的幂，默认 4096）
  --pool N            缓冲池帧数（默认 64）
  --replacer NAME     替换策略 LRU|FIFO|CLOCK（默认 LRU）
  --log LEVEL         日志级别 debug|info|warn|error|off（默认 info）
  --log-console       日志同时输出到控制台
  --lock-timeout MS   锁等待超时毫秒（默认 5000）
  --no-journal        不写事务审计日志
  --checkpoint-on-commit  每次提交都把数据文件落盘（更安全，但更慢）
  --auth              启用访问控制（需登录；首次自动创建管理员 root，空口令）
  -u, --user NAME     登录用户名（配合 --auth）
  -p, --password PW   登录口令（配合 --auth；交互模式省略则提示输入）
  -v, --verbose       打印执行计划、算子调用次数、事务号
  --show-plan         只编译并打印计划，不真正执行
  --echo              回显每条语句
  --timing            打印每条语句耗时
  --stats             退出前打印缓冲池统计
  -h, --help          帮助
```

退出码：`0` 全部成功；`1` 存在编译或执行失败；`2` 用法错误。

```powershell
# 交互式终端
.\build\cella_db\cella_db.exe --data .\mydb

# 跑脚本 + 打印计划 + 缓冲池统计
.\build\cella_db\cella_db.exe --data .\mydb -f cella_db\sql\demo_query.sql --stats

# 小缓冲池观察淘汰行为
.\build\cella_db\cella_db.exe --data .\mydb --pool 4 --replacer CLOCK --stats -f cella_db\sql\demo_basic.sql
```

### REPL 元命令

| 命令 | 作用 |
| --- | --- |
| `\?` / `\h` | 帮助 |
| `\q` | 退出 |
| `\d` | 列出所有表（含表号与首数据页） |
| `\d 表名` | 显示表结构 |
| `\l` | 列出所有数据库（等价 `SHOW DATABASES;`） |
| `\plan <SQL>` | 只编译并打印计划（优化前 / 优化后对比） |
| `\stats` | 缓冲池统计（命中率、淘汰、page_alloc/free） |
| `\locks` | 当前锁表（持有者 / 等待者） |
| `\waitfor` | 实时等待图（死锁检测依据） |
| `\txn` | 事务表与提交/回滚计数 |
| `\checkpoint` | 立即把数据文件落盘（存盘点） |
| `\whoami` | 显示当前登录用户与角色 |
| `\users` | 列出所有用户（等价 `SHOW USERS;`） |
| `\passwd <新口令>` | 修改自己的口令（等价 `SET PASSWORD = '...';`） |
| `\timing on\|off` | 打印每条语句耗时 |
| `\echo on\|off` | 回显每条语句 |

---

## 3. SQL 方言（沿用编译器方言）

| cella | 标准 SQL | | cella | 标准 SQL |
| --- | --- | --- | --- | --- |
| `get` | SELECT | | `join` / `on` | JOIN / ON |
| `in` | FROM | | `left` / `right` / `middle` | 外连接 / 内连接 |
| `limit` | WHERE | | `union` | UNION |
| `grouped` | GROUP BY | | `distinct` | DISTINCT |
| `having` | HAVING | | `as` | AS |
| `ordered` | ORDER BY | | `among` | LIMIT 行数 |
| `page 页码, 每页行数` | 分页 | | `is [not] null` | IS [NOT] NULL 判空 |

事务控制（编译器不识别，由会话拦截）：`BEGIN;` / `COMMIT;` / `ROLLBACK;`
（也接受 `START TRANSACTION` / `END`）。

建表可声明列级主键（隐含 NOT NULL，唯一性由执行层校验，冲突报 `DB-516`）：

```sql
CREATE TABLE s(id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);
CREATE TABLE c(code VARCHAR(8) NOT NULL PRIMARY KEY, title TEXT);  -- 约束顺序任意
```

每张表还有一个**只读伪列 `rowid`**（物理行标识，不透明整数），可用于投影 / 条件 / 排序：

```sql
get rowid, id, name in t;                 -- 同值重复行也能区分
delete in t limit rowid = 393216;         -- 精确删掉其中某一行（全列匹配做不到）
update t set name = 'x' limit rowid = 393216;
```

`rowid` 不可声明为列名、不可作为 INSERT 列清单或 UPDATE 的 SET 目标；`UPDATE` 后该行 rowid 会变
（删旧+插新），删除后槽位可能被复用 —— 客户端应在同一持锁事务内 fetch → 改，改完重新取一次 rowid。

多库控制（同样由会话拦截，编译器不识别）：

```sql
CREATE DATABASE school;   -- 建 <data_dir>/school.db（已存在 → DB-514）
USE school;               -- 切换当前库（事务中 → DB-513）
SHOW DATABASES;           -- 列出所有库（单列查询结果）
DROP DATABASE school;     -- 软删除：改名 <db>.db.dropped-<时间戳>，改回即恢复（当前库/启动库 → DB-515）
```

库 = `<data_dir>/<库名>.db` 一个自包含文件（默认库 `main`）。跨库并发请用多个引擎实例（多进程）。

访问控制（同样由会话拦截；默认关闭，`--auth` 开启，详见 [docs/AUTH.md](docs/AUTH.md)）：

```sql
CREATE USER alice IDENTIFIED BY 'pwd';   -- 口令可省略
DROP USER alice;
SHOW USERS;                              -- user | admin | created_at
SET PASSWORD = 'newpwd';                 -- 改自己的；SET PASSWORD FOR alice = '...' 由管理员代改

GRANT  get, insert ON main.student TO alice;   -- 单表读 + 写
GRANT  all          ON main.*       TO alice; -- 整库
GRANT  get          ON *.*          TO reporter;  -- 全局只读
GRANT  admin                        TO dba;    -- 提升管理员
REVOKE insert       ON main.student FROM alice;
SHOW GRANTS;                             -- 或 SHOW GRANTS FOR alice;
```

认证启用而未登录时任何语句都被拒（`DB-806`）；管理员全放行；系统表 `cella_catalog` 读放行。
`cella_auth` 是保留库名，不出现在 `SHOW DATABASES` 里。

> 注意：`GROUP BY`（`grouped`）在本方言里**没有聚合函数**（未定义 COUNT/SUM），
> 因此实现为「按分组键去重，每组保留首行」，配合 `having` 使用。
> 已知简化与边界见 [docs/INTEGRATION.md §6](docs/INTEGRATION.md)。

---

## 4. 错误码

| 段 | 码 | 说明 |
| --- | --- | --- |
| 编译器 | `LEX-1xx` / `SYN-2xx` / `SEM-3xx` / `PLN-4xx` | 词法 / 语法 / 语义 / 计划（原样透传） |
| 目录·执行 | `DB-501` | SQL 编译失败（携带编译器诊断原文） |
| | `DB-502` / `DB-503` | 表不存在 / 表已存在 |
| | `DB-504` / `DB-512` | 列不存在 / 系统表禁止修改（只读） |
| | `DB-516` | 主键冲突（唯一性被破坏） |
| | `DB-513` / `DB-514` / `DB-515` | 事务中禁切库 / 库不存在·已存在·名非法 / 目标库不可删除 |
| | `DB-505` / `DB-506` / `DB-507` / `DB-508` | 类型不匹配 / NOT NULL 违约 / 值个数不符 / 文本超长 |
| | `DB-510` / `DB-511` | 记录超页 / 除零 |
| | `DB-520` | 存储层返回失败（消息里附原始存储码） |
| 事务·并发 | `DB-601` / `DB-602` | 无活动事务 / 重复 BEGIN |
| | `DB-603` / `DB-604` / `DB-605` | 事务已中止 / 死锁（牺牲者）/ 锁等待超时 |
| 目录·会话 | `DB-701` / `DB-702` / `DB-703` / `DB-704` | 目录文件错 / 会话状态错 / 未实现 / 内部错误 |
| 访问控制 | `DB-801` / `DB-802` / `DB-803` | 认证失败 / 权限不足 / 用户不存在·已存在·名非法 |
| | `DB-804` / `DB-805` / `DB-806` | 最后一个管理员 / 无权授予（需管理员）/ 未登录 |

---

## 5. C++ API（30 秒速览）

```cpp
#include "cella/db/engine/db_engine.h"

cella::db::EngineConfig cfg;
cfg.data_dir = "./mydb";
cfg.pool_size = 64;
cfg.replacer = "LRU";

cella::db::DbEngine engine;
if (!engine.Open(cfg).ok()) { /* 处理错误 */ }

cella::db::ScriptReport report;
engine.default_session().Execute(
    "CREATE TABLE t(id INT NOT NULL, v VARCHAR(16));"
    "INSERT INTO t VALUES (1,'a'),(2,'b');"
    "get id, v in t ordered id desc;",
    &report);

for (const auto& s : report.statements) {
    std::cout << "[" << s.kind << "] ";
    if (!s.status.ok())            std::cout << s.status.ToString() << "\n";
    else if (s.result.IsQuery())   std::cout << "\n" << s.result.ToText() << "\n";
    else                           std::cout << s.result.Summary() << "\n";
}

engine.Close();
```

完整示例见 [`examples/api_quickstart.cpp`](examples/api_quickstart.cpp)；
多线程与死锁演示见 [`examples/concurrency_demo.cpp`](examples/concurrency_demo.cpp)。

### 关键类型

| 类型 | 说明 |
| --- | --- |
| `DbStatus` | `[[nodiscard]]` 统一返回状态（错误码 + 说明） |
| `ScriptReport` | 一次调用的整体汇报：`statements[]`（每条含状态/结果/计划/耗时/事务号）+ 汇总 |
| `StatementOutcome` | 单条语句：`kind`、`compiled`、`executed`、`auto_committed`、`rolled_back_here`、`implicit_commit`、`notice`、`plan_text`、`original_plan_text`、`operator_calls` |
| `QueryResult` | `columns[]` + `rows[][]` + `affected` + `tag`，`ToText()` 输出对齐表格 |

---

## 6. 测试

```powershell
# 全部用例
.\build\cella_db\cella_db_tests.exe

# 按名字过滤（中文子串）
.\build\cella_db\cella_db_tests.exe 事务

# 同时写日志文件
.\build\cella_db\cella_db_tests.exe --log .\build\test_report.log
```

当前：**96 用例 / 848 断言 / 0 失败**，明细见 [docs/TEST_REPORT.md](docs/TEST_REPORT.md)。

---

## 7. 目录

```
include/cella/db/
  common/   db_status.h  db_logger.h  time_util.h  value_bridge.h
  catalog/  catalog_manager.h
  exec/     executor.h  expr_eval.h  row_set.h  query_result.h
  txn/      transaction.h  lock_manager.h
  engine/   db_engine.h  sql_text.h
src/cella/db/   与头文件一一对应的实现
sql/            demo_basic.sql  demo_query.sql  demo_txn.sql
examples/       api_quickstart.cpp  concurrency_demo.cpp
tests/          mini_test.h  test_util.h  test_main.cpp  test_{common,catalog,exec,txn,concurrency,e2e}.cpp
docs/           INTEGRATION.md  ARCHITECTURE.md  TEST_REPORT.md
```
