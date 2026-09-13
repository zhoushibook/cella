# cella —— 编译器 × 操作系统 × 数据库层次 的完整整合系统

把已有的**SQL 编译器前端**（`cella_sql`）与**页式存储 / 缓冲池**（`cella_storage`）
通过一个新增的**数据库层**（`cella_db`）深度整合，形成一条可跑通的完整链路：

```
SQL 文本 ─▶ 词法 ─▶ 语法 ─▶ 语义 ─▶ 执行计划 ─▶ 计划优化 ─▶ 执行调度 ─▶ 存储存取 ─▶ 结果返回
           └──────────── cella_sql ────────────┘ └──── cella_db ────┘ └─ cella_storage ─┘
```

四个模块可**各自独立构建**，也可由本目录的聚合工程一起构建。依赖严格单向、无环。

---

## 1. 快速开始

前置：Visual Studio 2022（MSVC）+ CMake + Ninja（本机均不在 PATH，脚本会通过 VS DevShell 建环境）。

```powershell
# ① 一键构建 + 全量验证（构建 → 三层测试 + 客户端测试 → 端到端脚本 → 示例）
powershell -ExecutionPolicy Bypass -File run_all.ps1

# ② 只构建
powershell -ExecutionPolicy Bypass -File build.ps1

# ③ 进入交互式 SQL 终端
.\build\cella_db\cella_db.exe --data .\mydb

# ④ 启动图形客户端（浏览器打开 http://127.0.0.1:8080）
.\build\cella_client\cella_web.exe --data .\mydb --port 8080
```

交互式终端示例：

```
cella> CREATE TABLE student(id INT NOT NULL, name VARCHAR(16), score DOUBLE);
CREATE TABLE student
cella> INSERT INTO student VALUES (1,'Alice',88.5),(2,'Bob',76.0);
INSERT 0 2
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

---

## 2. 目录结构

```
.
├── CMakeLists.txt              聚合工程：add_subdirectory × 4
├── build.ps1                   构建脚本（VS DevShell + Ninja）
├── run_all.ps1                 一键构建 + 全量验证
├── README.md                   本文件
│
├── cella_sql/                  SQL 编译器前端（原有，保持功能与结构）
│   ├── include/cella/          lexer/parser/semantic/planner/optimizer/printer
│   ├── src/                    实现 + main.cpp（CLI）
│   └── tests/                  29 个回归用例（正向 12 + 负向 17）+ golden
│
├── cella_storage/              页式存储 + 缓冲池（原有，源码零改动）
│   ├── include/cella/storage/  IStorage / Page / BufferPool / TableHeap / ...
│   ├── src/                    实现
│   ├── tests/                  30 用例 / 4620 断言
│   └── docs/                   存储层自己的接口契约与设计文档
│
└── cella_db/                   【本次新增】数据库层 + 整合层
    ├── include/cella/db/       common / catalog / exec / txn / engine
    ├── src/                    实现
    ├── sql/                    3 个可执行演示脚本
    ├── examples/               API 速览 + 并发现场演示
    ├── tests/                  105 用例 / 970 断言
    └── docs/                   INTEGRATION.md / ARCHITECTURE.md / TEST_REPORT.md

cella_client/                   【图形客户端】内嵌 HTTP 服务 + 浏览器 SPA
    ├── include/cella/client/   net（socket/HTTP/静态资源）/ api（json/router/sql_builder）/ server
    ├── src/                    实现 + main.cpp（cella_web 服务入口）
    ├── web/                    前端资源（纯 HTML/CSS/原生 ES Module，无构建步骤）
    ├── tests/                  25 用例（JSON/HTTP/SQL 生成/API 端到端）
    └── docs/                   PLAN_web_client.md（设计与实现约定）
```

---

## 3. 构建产物

| 产物 | 说明 |
| --- | --- |
| `build/cella_db/cella_db.exe` | **完整系统 CLI**（交互 REPL / 脚本 / 元命令 / 表格化输出） |
| `build/cella_db/cella_db_tests.exe` | 整合层测试（105 用例 / 970 断言） |
| `build/cella_db/api_quickstart.exe` | 30 秒 C++ API 速览 |
| `build/cella_db/concurrency_demo.exe` | 并发锁 / 死锁检测现场演示 |
| `build/cella_client/cella_web.exe` | **图形客户端服务**（浏览器打开 `http://127.0.0.1:8080`） |
| `build/cella_client/cella_client_tests.exe` | 客户端层测试（25 用例：JSON/HTTP/SQL 生成/API 端到端） |
| `build/cella_sql/cella_sql.exe` | 原编译器 CLI（保留，`-l/-a/-s/-p/-o/--all` 行为不变） |
| `build/cella_storage/*.exe` | 存储层 demo / quickstart / crud_flow / 测试（保留） |

---

## 4. 验证结果（2026-09-10，Release，MSVC 14.41 + Ninja）

| 套件 | 用例 | 断言 | 失败 |
| --- | --- | --- | --- |
| 编译器回归 | 33 | 33 项 golden 比对 | 0 |
| 存储层单元测试（原有） | 30 | 4620 | 0 |
| 整合层测试（新增） | 105 | 970 | 0 |
| 端到端 SQL 脚本 | 70 条语句 | — | 4（脚本刻意演示的错误路径） |

构建零 error / 零 warning（MSVC `/W4 /WX`）。详见 [cella_db/docs/TEST_REPORT.md](cella_db/docs/TEST_REPORT.md)。

---

## 5. 文档导航

| 文档 | 内容 | 读者 |
| --- | --- | --- |
| [cella_db/docs/INTEGRATION.md](cella_db/docs/INTEGRATION.md) | **接口与依赖梳理**：四道接缝、算子→存储映射表、数据流、内存管理与调度衔接、改动清单、取舍 | 评审 / 整合负责人 |
| [cella_db/docs/ARCHITECTURE.md](cella_db/docs/ARCHITECTURE.md) | 数据库层设计：执行模型、表达式求值、事务与并发控制、错误与日志、扩展点 | 维护者 |
| [cella_db/docs/TEST_REPORT.md](cella_db/docs/TEST_REPORT.md) | 用例清单 + 实测输出 + 性能观测 + 既有缺陷修复记录 | 评审 |
| [cella_db/README.md](cella_db/README.md) | 数据库层使用手册（CLI / 元命令 / 方言 / 错误码 / API） | 使用者 |
| [cella_sql/README.md](cella_sql/README.md) | 编译器前端规格（文法、错误码、golden 契约） | 编译器维护者 |
| [cella_storage/docs/INTERFACE_CONTRACT.md](cella_storage/docs/INTERFACE_CONTRACT.md) | 存储层对外契约 | 存储维护者 |

---

## 6. 本整合做了什么（摘要）

**要求 1 · 模块整合**
梳理出四道接缝（计划节点 / IStorage / 类型桥 / 目录桥），把 `CELLA_PlanNode`
升级为**可执行 IR**（附加执行期字段，打印与 golden 零变化），
执行器按算子映射调用 `IStorage` 完成内存管理与存取；会话/事务/锁三层调度衔接底层机制。

**要求 2 · 数据库层次**
持久化文件组织（存储层页式，**元数据以系统表 `cella_catalog` 存于数据文件内**）、
表结构与元数据管理（`CatalogManager` + 系统表行读写，可被 SQL 查询）、
记录增删改查（含 `UPDATE = 删旧+插新`）、
基本事务处理（undo 日志 + 语句级原子性 + DDL 隐式提交）与并发控制（表级严格 2PL + 死锁检测）。
多库：`CREATE DATABASE / DROP DATABASE（软删除）/ USE / SHOW DATABASES`，库 = 自包含单文件。
访问控制（默认关，`--auth` 开启）：认证（用户 / 口令）+ 授权（`GRANT` / `REVOKE`，表级 / 库级 / 全局）。

**要求 3 · 整体系统**
打通「查询输入 → 编译解析 → 计划优化 → 执行调度 → 数据存取 → 结果返回」全链路；
分层接口清晰、原模块功能与结构保持、错误码分段（编译器诊断 / DB-5xx / DB-6xx / DB-7xx）、
三层日志（存储层 / 数据库层 / 事务审计），系统可运行、可测试、可扩展。
