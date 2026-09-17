# cella —— 关系型数据库系统

**一个零第三方依赖的数据库系统。** 「SQL 编译器前端」「页式存储 / 缓冲池 / B+ 树索引」
「数据库执行层」「图形客户端」四层：

```
SQL 文本 ─▶ 词法 ─▶ 语法 ─▶ 语义 ─▶ 逻辑计划 ─▶ 计划优化 ─▶ 执行调度 ─▶ 存储存取 ─▶ 结果返回
           └──────────── cella_sql ────────────┘ └──── cella_db ────┘ └─ cella_storage ─┘
                                                    ▲
                                              cella_client（HTTP + 浏览器 SPA）
```

四个模块**依赖严格单向、无环**，既可**各自独立构建**，也可由仓库根的聚合工程一起构建。

| 模块 | 定位 | 规模（.h/.cpp） |
| --- | --- | --- |
| **`cella_sql`** | SQL 编译器前端：词法 / 语法 / 语义 / 计划 / 优化 / 打印 | 17 文件 · 6788 行 |
| **`cella_storage`** | 页式存储 + 缓冲池 + 记录管理 + B+ 树索引 | 75 文件 · 6391 行 |
| **`cella_db`** | 数据库层：目录 / 执行器 / 事务 / 并发 / 访问控制 / WAL + 统一 CLI | 61 文件 · 23593 行 |
| **`cella_client`** | 图形客户端：内嵌 HTTP 服务 + 浏览器 SPA（无构建步骤） | 28 文件 · 4249 行 + 19 前端资源 |

技术约束：**C++17、零第三方依赖、全中文注释**；MSVC `/W4 /WX` 与 g++ `-Wall -Wextra -Werror` 下
**零 error / 零 warning**。代码风格：编译器层 `cella::` 用 4 空格；存储层 `cella::storage`、数据库层
`cella::db` 用 2 空格、成员变量尾部 `_`。

---

## 1. 快速开始

前置：**Visual Studio 2022（MSVC）+ CMake + Ninja**。本机工具链均不在 PATH，
`build.ps1` 会自动通过 VS DevShell 建好环境（**注意：MSBuild 生成器会崩，必须用 Ninja**）。

```powershell
# ① 一键构建 + 全量验证（构建 → 编译器回归 → 存储 → 整合 → 访问控制 → 客户端 → 前端自测 → e2e → 示例）
powershell -ExecutionPolicy Bypass -File run_all.ps1

# ② 只构建（配置 + 全量编译，日志 build/build.log）
powershell -ExecutionPolicy Bypass -File build.ps1

# ③ 进入交互式 SQL 终端
.\build\cella_db\cella_db.exe --data .\mydb --pool 512

# ④ 启动图形客户端（浏览器打开 http://127.0.0.1:8080）
.\build\cella_client\cella_web.exe --data .\mydb --port 8080
```

---

## 2. cella SQL 方言与示例

cella 使用一套自定的英文关键字：

| cella | SQL | | cella | SQL |
| --- | --- | --- | --- | --- |
| `get` | SELECT | | `ordered` | ORDER BY |
| `in` | FROM | | `among` | LIMIT |
| `limit` | **WHERE** | | `grouped` | GROUP BY |
| `page 页码[,每页]` | 原生分页 | | `having` | HAVING |
| `join` / `middle` | JOIN / INNER JOIN | | `rowid` | 行伪列 |

```
cella> CREATE TABLE student(id INT NOT NULL PRIMARY KEY, name VARCHAR(16), score DOUBLE);
cella> INSERT INTO student VALUES (1,'Alice',88.5),(2,'Bob',76.0);
cella> get id, name, score in student ordered score desc;
id | name  | score
---+-------+------
1  | Alice | 88.5
2  | Bob   | 76
(2 行)
cella> BEGIN; UPDATE student SET score = 90 limit id = 2; ROLLBACK;
cella> \d
表数量: 1
  #1 student (3 列, 首数据页 1)
```

元命令、完整方言细则、错误码分段与 C++ API 见 [cella_db/README.md](cella_db/README.md)。

---

## 3. 目录结构

```
.
├── CMakeLists.txt              聚合工程：add_subdirectory × 4
├── build.ps1                   构建脚本（VS DevShell + Ninja）
├── run_all.ps1                 一键构建 + 全量验证（判定依据是退出码 + 程序落盘产物）
├── README.md                   本文件
│
├── cella_sql/                  【编译器前端】词法 → 语法 → 语义 → 计划 → 优化 → 打印
│   ├── include/cella/          lexer / parser / ast / semantic / planner / optimizer / printer
│   ├── src/                    实现 + main.cpp（CLI：-l / -a / -s / -p / -o / --all）
│   └── tests/                  sql/ 58 个回归用例（正向 19 + 负向 39）+ expected/ golden 比对
│
├── cella_storage/              【存储层】页式存储 + 缓冲池 + 记录 + B+ 树索引
│   ├── include/cella/storage/  api/(IStorage·FileStorage·StorageFactory) common/ table/ 导出；
│   │                           disk/ page/ buffer/ record/ index/ 为实现
│   ├── src/                    实现
│   ├── tests/                  storage_tests（58 用例 / 10806 断言）+ 诊断 / 演示程序
│   └── docs/                   INTERFACE_CONTRACT / EXTENSIBILITY 等 6 份设计文档
│
├── cella_db/                   【数据库层 + 整合层】
│   ├── include/cella/db/       common / catalog / exec / txn / auth / wal / engine
│   ├── src/                    实现 + main.cpp（cella_db CLI）
│   ├── sql/                    5 个可执行演示脚本（basic / query / functions / lang_features / txn）
│   ├── examples/               api_quickstart（API 速览）+ concurrency_demo（锁 / 死锁现场演示）
│   ├── tests/                  cella_db_tests（278 用例 / 4230 断言）
│   └── docs/                   INTEGRATION / ARCHITECTURE / TEST_REPORT / PLAN_* 等
│
└── cella_client/               【图形客户端】内嵌 HTTP 服务 + 浏览器 SPA
    ├── include/cella/client/   net(socket·HTTP·静态资源) / api(json·router·sql_builder) / server
    ├── src/                    实现 + main.cpp（cella_web 服务入口）
    ├── web/                    前端资源（纯 HTML/CSS/原生 ES Module，无构建步骤）+ _selftest/ 自测页
    ├── tests/                  cella_client_tests（32 用例 / 227 断言）
    └── docs/                   PLAN_web_client.md（设计与实现约定）
```

---

## 4. 构建产物

| 产物 | 说明 |
| --- | --- |
| `build/cella_db/cella_db.exe` | **完整系统 CLI**（交互 REPL / 脚本 / 元命令 / 表格化输出 / `EXPLAIN`） |
| `build/cella_db/cella_db_tests.exe` | 整合层测试（278 用例 / 4230 断言） |
| `build/cella_db/api_quickstart.exe` | 30 秒 C++ API 速览 |
| `build/cella_db/concurrency_demo.exe` | 并发锁 / 死锁检测现场演示 |
| `build/cella_client/cella_web.exe` | **图形客户端服务**（浏览器打开 `http://127.0.0.1:8080`） |
| `build/cella_client/cella_client_tests.exe` | 客户端层测试（JSON / HTTP / SQL 生成 / API 端到端 / 访问控制） |
| `build/cella_sql/cella_sql.exe` | 编译器 CLI（`-l/-a/-s/-p/-o/--all` 行为不变） |
| `build/cella_storage/*.exe` | 存储层测试 / 诊断 / 演示（storage_tests、bptree_diag、crud_flow、quickstart、storage_demo） |

---

## 5. 验证结果（2026-09-15，Release，MSVC 14.4x + Ninja）

| 套件 | 用例 | 断言 / 比对 | 失败 |
| --- | --- | --- | --- |
| 编译器回归 | 58（正向 19 + 负向 39） | 20 份 golden 计划比对 | 0 |
| 存储层单元测试 | 58 | 10806 | 0 |
| 整合层测试 | 278 | 4230 | 0 |
| 访问控制冒烟 | 7 步 | — | 0 |
| 客户端层测试 | 32 | 227 | 0 |
| 前端自测（无头浏览器） | 7 页 | 246 | 0 |
| 端到端 SQL 脚本 | 68 条语句 | — | 0（另有脚本刻意演示的错误路径） |

全量构建零 error / 零 warning。一键复现：`powershell -ExecutionPolicy Bypass -File run_all.ps1`。
详细用例清单与实测输出见 [cella_db/docs/TEST_REPORT.md](cella_db/docs/TEST_REPORT.md)。

---

## 6. 系统能力总览

**编译器层**：手写词法 / 递归下降语法 / 语义分析（名字解析、类型检查、约束校验）→ 逻辑计划 →
基于代价的优化（谓词下推、索引选择、投影剪枝）→ 计划打印；诊断错误码分段 `LEX-* / SYN-* / SEM-* / PLN-*`。

**存储层**：4 KB 页式组织（页头 32 B + 槽目录 + 从页尾向左摆的记录区，删除走墓碑）
+ 可插拔缓冲池（LRU / FIFO / CLOCK，脏页写回、`PageGuard` RAII 自动 unpin）
+ `IStorage` 抽象（`FileDiskManager` 落盘 / `MemDiskManager` 测试）+ 前后端自洽的 **B+ 树索引**
（数据在叶子、叶子 `right_sibling` 串链表；键编码保证"字节字典序 == 逻辑序"）。

**数据库层**：`CatalogManager` 把**元数据作为系统表存进数据文件本身**（可被 SQL 直接查询）；
执行器按计划算子映射调用 `IStorage` 完成增删改查（`UPDATE = 删旧 + 插新`）；
**表级严格 2PL + 死锁检测** 的并发控制；undo 补偿日志 + 语句级原子性 + DDL 隐式提交的事务；
**WAL 预写日志**（进程崩溃可恢复）；**多库**（`CREATE/DROP DATABASE`、`USE`，库 = 自包含单文件）；
**访问控制**（默认关，`--auth` 开启：认证 + `GRANT`/`REVOKE` 表级 / 库级 / 全局授权）。

**客户端层**：进程内嵌 HTTP 服务 + 零框架前端 SPA，支持库表树导航、SQL 编辑器、
数据网格浏览与 Navicat 式暂存编辑（改动单事务提交）。

---

## 7. 设计要点与取舍（摘要）

完整的接口梳理、四道接缝、算子→存储映射、改动清单与取舍记录见
[cella_db/docs/INTEGRATION.md](cella_db/docs/INTEGRATION.md)。要点：

- **四道接缝**：计划节点（`CELLA_PlanNode` 升级为携带执行期字段的**可执行 IR**，打印与 golden 零变化）、
  `IStorage`（执行器只依赖抽象）、类型桥（编译器类型 ↔ 存储列类型）、目录桥（`CatalogManager` ↔ 系统表）。
- **分层不穿透**：存储层**不认识"索引"**（`IStorage` 无任何索引方法），B+ 树只依赖 `BufferPoolManager`
  抽象；索引根页号、列清单、唯一标记全在数据库层的 `cella_index` 系统表，靠 `Attach(root)` 恢复。
- **堆表而非聚簇表**：表按插入顺序追加，主键索引与二级索引同构（叶子都是「键 + 行定位」），
  路线同 PostgreSQL（索引存物理位置）——代价是主键点查也要回表一次。
- **有意简化**：B+ 树删除采用墓碑式（不合并节点、不回收键字节）；索引页暂不参与空闲页回收。
- **原模块零破坏**：`cella_sql` / `cella_storage` 的对外签名与语义保持不动，整合只做增量或缺陷修复，
  修复逐条记入 [cella_db/docs/INTEGRATION.md](cella_db/docs/INTEGRATION.md) §5。

---

## 8. 文档导航

| 文档 | 内容 |
| --- | --- |
| [cella_db/docs/INTEGRATION.md](cella_db/docs/INTEGRATION.md) | **接口与依赖梳理**：四道接缝、算子→存储映射、数据流、内存管理与调度衔接、改动清单、取舍 |
| [cella_db/docs/ARCHITECTURE.md](cella_db/docs/ARCHITECTURE.md) | 数据库层设计：执行模型、表达式求值、事务与并发、错误与日志、扩展点 |
| [cella_db/docs/TEST_REPORT.md](cella_db/docs/TEST_REPORT.md) | 用例清单 + 实测输出 + 性能观测 + 既有缺陷修复记录 |
| [cella_db/README.md](cella_db/README.md) | 数据库层使用手册（CLI / 元命令 / 方言 / 错误码 / C++ API） |
| [cella_sql/README.md](cella_sql/README.md) | 编译器前端规格（文法、AST、错误码、golden 契约） |
| [cella_storage/README.md](cella_storage/README.md) | 存储层使用与设计总览 |
| [cella_storage/docs/INTERFACE_CONTRACT.md](cella_storage/docs/INTERFACE_CONTRACT.md) | 存储层对外接口契约 |
| [cella_client/docs/PLAN_web_client.md](cella_client/docs/PLAN_web_client.md) | 图形客户端设计与实现约定（含 HTTP API、访问控制、实测记录） |
