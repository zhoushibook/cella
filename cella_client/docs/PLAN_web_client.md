# 实现清单：cella 图形客户端（Navicat 风格）

> 目标：为 cella 三模块系统补上一个**简约、美观、实用**的图形客户端，
> 让「建表 → 灌数据 → 写查询 → 看结果 → 查诊断」全程脱离命令行。
> 状态：**规划稿，未动工**。按阶段推进，每个阶段独立可构建、可验收、可回退。

---

## 0. 决策速览

| # | 决策点 | 结论 | 理由 |
| --- | --- | --- | --- |
| D1 | 客户端形态 | **内嵌 HTTP 服务 + 浏览器单页应用** | 守住「零第三方依赖」底线；网格/主题/信息密度这类 Navicat 的核心体验，Web 技术栈是成本最低的实现路径 |
| D2 | 代码落点 | **新增顶层模块 `cella_client/`** | 贴合现有「三模块各自独立可构建」结构；`cella_db`/`cella_sql`/`cella_storage` **零改动** |
| D3 | 首版范围 | **核心查询套件** | 覆盖日常 80% 使用场景，是最扎实的第一版；表设计器/导入导出/ER 图留作后续 |
| D4 | 线程模型 | thread-per-connection + `engine_gate_` 全局串行 | `IStorage` 非线程安全，`storage_mutex_` 本来就是串行的；并行只用于静态资源与 JSON 编解码 |
| D5 | 会话模型 | **单会话**：一个服务进程 = 一个连接 = 一个 `Session` | `current_db_` 是引擎级状态，多会话带不来真并发，只会让事务归属变歧义 |
| D6 | 库切换 | v1 单引擎 + `USE` 语义；`EnginePool` 列为 v1.1 可选增强 | 复用现有语义，风险最低；多库真并发在引擎层就要求多实例 |
| D7 | 行编辑定位 | **全列匹配**（把原行所有列拼成 `limit` 条件） | 方言**没有主键概念**，也没有 `rowid`。这是硬约束，详见 §6.1 |
| D8 | 总行数 | 懒计数 + 前端缓存，不追求实时精确 | 方言**没有 COUNT 聚合**，精确总数必须全表扫，详见 §6.2 |

---

## 1. 目标与非目标

### 想做成的

1. **像 Navicat 那样用**：左边一棵「库 → 表」树，双击表看数据，右键新建查询，写 SQL 按 `Ctrl+Enter` 出表格。
2. **好看**：浅色为默认主题（贴合当前 IDE），紧凑排版、克制的配色、清晰的层次，不花哨。
3. **好用**：结果可排序、可复制、可导出；错误直接指到行列；长查询有加载态；键盘可达。
4. **能看见系统内部**：把 `\stats` / `\locks` / `\waitfor` / `\txn` / `\plan` 这些 CLI 诊断能力图形化 —— 这是 cella 相比通用工具**独有的价值**。

### 明确不做（首版）

- 用户/权限管理（引擎里没有这个概念）
- 备份恢复、定时任务、数据传输向导
- 多连接管理（一个服务进程 = 一个数据目录）
- 跨平台打包分发（先保证 Windows 本机跑通；socket 层预留 POSIX 分支）

---

## 2. 为什么是「内嵌 HTTP + 浏览器」

三条路都评估过：

| 方案 | 优势 | 否决/采纳理由 |
| --- | --- | --- |
| Win32/Direct2D 自绘 | 真·桌面程序，形态最像 Navicat | 网格控件的虚拟滚动、列宽拖拽、内联编辑、单元格选择区、对话框全要手写，**UI 代码量会远超业务代码**，且纯 Win32 的观感很难做到「美观」 |
| Qt / FLTK / wxWidgets | UI 工作量最小 | 打破「C++17 + 零第三方依赖 + MSVC `/W4 /WX` 单命令构建」的既有约束，构建链和仓库体积明显变重 |
| **内嵌 HTTP + 浏览器** ✅ | 零新依赖，UI 完全自由，迭代最快 | 代价是要新写一个极简 HTTP 服务（Winsock2 约 400 行），以及不自带原生窗口 |

关键判断：**这个项目的技术资产在引擎层，客户端要做的是「把引擎能力暴露好」**。
用 Web 技术栈，能把精力全部投在信息架构、交互细节和视觉打磨上，而不是和 `WM_PAINT` 搏斗。

---

## 3. 总体架构

### 3.1 分层

```
┌──────────────────────────────────────────────────────────────┐
│  浏览器（纯 HTML/CSS/原生 JS，无构建步骤）                     │
│  左侧树 │ 标签区(查询/数据/结构) │ 结果网格 │ 状态栏            │
└───────────────────────────┬──────────────────────────────────┘
                            │  HTTP/1.1 + JSON  (127.0.0.1:8080)
┌───────────────────────────┴──────────────────────────────────┐
│  cella_client  —— 新增模块                                    │
│                                                              │
│   net/    Socket(Winsock2) │ HttpServer │ Router │ 静态资源    │
│   api/    Json 编解码 │ REST handlers │ SQL 生成器 │ 错误映射   │
│   server/ ApiService（engine_gate_ 串行化）│ 启动参数           │
└───────────────────────────┬──────────────────────────────────┘
                            │  进程内直接调用（不是网络！）
┌───────────────────────────┴──────────────────────────────────┐
│  cella_db_core  （已有，零改动）                              │
│  DbEngine → Session → Executor → IStorage                     │
│           ↑ 复用 CatalogManager 的结构化元数据                 │
└──────────────────────────────────────────────────────────────┘
```

**关键点**：服务端和引擎是**同一个进程**。HTTP 只是「前端与引擎之间的进程内调用」的一层薄壳，
不引入网络传输协议、序列化框架、端口协商这些额外复杂度。

### 3.2 目录规划

```
cella_client/
├── CMakeLists.txt
├── README.md                      使用手册（编译、启动、界面导览、快捷键）
├── include/cella/client/
│   ├── net/
│   │   ├── socket.h               RAII socket 封装（Winsock2 / POSIX 双分支）
│   │   ├── http_types.h           HttpRequest / HttpResponse / 状态码 / MIME
│   │   ├── http_parser.h          请求行 + 头部 + Content-Length 正文解析
│   │   ├── http_server.h          监听 / accept / 每连接一线程 / 静态资源
│   │   └── static_files.h         web/ 目录映射 + 路径穿越防护
│   ├── api/
│   │   ├── json.h                 JsonValue / 序列化 / 极简解析（仅请求体用）
│   │   ├── json_result.h          QueryResult ⇄ JSON 的类型化转换
│   │   ├── sql_builder.h          分页/排序/筛选/行定位 → cella 方言 SQL
│   │   ├── error_map.h            编译诊断 + DbStatus → 统一错误 JSON
│   │   └── router.h               路由表 + 参数提取
│   └── server/
│       ├── api_service.h          全部 REST handler（持有 DbEngine）
│       └── server_config.h        端口 / 数据目录 / web 目录 / 参数解析
├── src/                           与 include 一一对应
├── web/                           前端资源（编译期路径注入，开发时改完刷新即生效）
│   ├── index.html
│   └── assets/
│       ├── app.css                设计令牌 + 组件样式（含深色主题）
│       └── js/
│           ├── api.js             fetch 封装 + 统一错误 Toast
│           ├── store.js           应用状态（当前库/打开的标签/结果缓存）
│           ├── editor.js          编辑器：高亮层 + 快捷键 + 格式化
│           ├── grid.js            结果网格：虚拟滚动 + 排序 + 选区 + 复制
│           ├── tree.js            库/表树 + 右键菜单
│           ├── panels.js          结构面板 / 诊断面板 / 执行计划视图
│           └── app.js             装配与路由
├── tests/                         客户端层测试（HTTP/JSON/路由/端到端）
└── docs/
    └── PLAN_web_client.md         本文件
```

### 3.3 线程与数据流

```
accept 线程 ──每连接 fork 一个 worker──▶  worker 线程
                                            │
              ┌─────────────────────────────┴──────────────────────────┐
              │ 1. 解析 HTTP（无锁，可并行）                            │
              │ 2. 静态资源 → 直接读盘返回（无锁，可并行）               │
              │ 3. /api/* → std::lock_guard<std::mutex> engine_gate_    │
              │              调用 DbEngine / Session（严格串行）        │
              │ 4. JSON 序列化 + 写回 socket（无锁，可并行）             │
              └────────────────────────────────────────────────────────┘
```

- **`engine_gate_` 与 `DbEngine::storage_mutex_` 同构**：既然存储访问本来就串行，
  在 API 层再串行一层不损失任何吞吐，却把「`current_db_` 被并发改写」这类问题的可能性直接归零。
- **锁顺序**：`engine_gate_` → `storage_mutex_`，与既有约定一致，不会引入新的死锁路径。
- 前端单页应用是**串行发请求**的（用户一次只操作一个东西），实际并发度极低。

---

## 4. 后端设计

### 4.1 极简 HTTP 服务器（零依赖）

只实现 SPA 客户端需要的子集，刻意不做「一个完整 HTTP 框架」：

| 支持 | 内容 |
| --- | --- |
| 方法 | `GET` / `POST`（够用；其余方法返回 405） |
| 版本 | HTTP/1.1，带 `Content-Length`；`Connection: keep-alive` 可选（v1 用 `close`，简单） |
| 请求 | 请求行 + 头部 + 定长正文（`Content-Length`）；**不支持 chunked**（前端不会发） |
| 响应 | 状态行 + `Content-Type` + `Content-Length` + `Cache-Control` |
| 编码 | 全链路 UTF-8；`application/json; charset=utf-8` / `text/html; charset=utf-8` |
| 安全 | 只绑 `127.0.0.1`；静态资源路径规范化后必须在 `web/` 之内（防 `../` 穿越） |
| 限制 | 请求头 ≤ 16 KB、正文 ≤ 8 MB、`recv` 超时 30 s（防挂死连接） |

Windows 细节（会被 `/W4 /WX` 卡住的点）：
`#define WIN32_LEAN_AND_MEAN` + `#define NOMINMAX`、`WSAStartup/WSACleanup` 生命周期、
`ws2_32` 链接（CMake `target_link_libraries(... PRIVATE ws2_32)`）、
`SOCKET` 是 `UINT_PTR` 不能和 `int` 混用（用 `INVALID_SOCKET` 判断）、
`socket` 长度参数用 `int` 不用 `socklen_t`。

### 4.2 JSON 编解码

自己写，约 300 行：

- **编码**：`JsonValue`（null/bool/number/string/array/object）→ 紧凑文本。
  字符串转义覆盖 `"` `\` `\b\f\n\r\t` 与 `< 0x20` 的 `\u00XX`；**中文直接输出 UTF-8 原文**（可读、省字节）。
  数字：整数走 `int64`，浮点用 `%.17g` 再裁剪尾零（保证 double 往返不丢精度）。
- **解码**：只用于请求体（结构固定、简单），支持对象/数组/字符串/数字/bool/null，不追求完备。

> 为什么不用第三方：这是项目红线。而且这里的 JSON 需求确实简单 —— 序列化为主、解析为辅。

### 4.3 REST API 契约

统一响应外形：

```jsonc
// 成功
{ "ok": true, "data": { /* ... */ } }
// 失败（引擎错误、参数错误、内部错误一律走这里）
{ "ok": false, "error": { "code": "DB-502", "message": "表不存在: student",
                          "line": 0, "col": 0, "detail": "【语义错误 SEM-301】..." } }
```

| 方法 | 路径 | 说明 | 落到引擎的什么能力 |
| --- | --- | --- | --- |
| `GET` | `/api/health` | 版本、数据目录、当前库、页大小/缓冲池配置、启动时长 | `EngineConfig` + `current_db()` |
| `GET` | `/api/databases` | 库列表（名称、是否当前库、文件大小） | `ShowDatabases()` + 目录扫描 |
| `POST` | `/api/databases/use` | 切换当前库 `{name}` | `UseDatabase()` |
| `POST` | `/api/databases/create` | 新建库 `{name}` | `CreateDatabase()` |
| `POST` | `/api/databases/drop` | 删库 `{name}`（软删除） | `DropDatabase()` |
| `POST` | `/api/query` | **核心**：执行 SQL `{sql, maxRows}` | `Session::Execute()` → `ScriptReport` |
| `POST` | `/api/plan` | 只编译返回计划 `{sql}` | `Session::CompileOnly()` |
| `GET` | `/api/catalog` | 表清单（表名、表号、列数、建表时间） | `CatalogManager::ListTables()` |
| `GET` | `/api/catalog/{table}` | 单表列定义（名/类型/长度/NOT NULL/序位） | `FindTable()` → `CatalogTable` |
| `GET` | `/api/tables/{table}/rows` | 分页取数 `?page=1&pageSize=100&sort=&order=` | 生成 `get ... ordered ... page N, M` |
| `GET` | `/api/tables/{table}/count` | 行数（懒统计，见 §6.2） | 生成 `get <首列> in <表>` 后计数 |
| `POST` | `/api/tables/{table}/rows` | 插入一行 `{values:{列:值}}` | `INSERT INTO ... VALUES (...)` |
| `PATCH` | `/api/tables/{table}/rows` | 改一行 `{keys:{...}, values:{...}}` | `UPDATE ... SET ... limit <全列匹配>` |
| `DELETE` | `/api/tables/{table}/rows` | 删一行 `{keys:{...}}` | `DELETE in ... limit <全列匹配>` |
| `POST` | `/api/tables/ddl` | 建表/删表（表设计器用，首版仅 `POST` 原始 DDL） | `Session::Execute()` |
| `POST` | `/api/txn/{begin\|commit\|rollback}` | 事务三件套 | `Session::Begin/Commit/Rollback` |
| `POST` | `/api/checkpoint` | 存盘点 | `DbEngine::Checkpoint()` |
| `GET` | `/api/diagnostics/{stats\|locks\|waitfor\|txn}` | 四项诊断 | `StatsText()` / `LockText()` / `WaitForGraphText()` / `TxnText()` |
| `GET` | `/api/session` | 会话状态（事务号、是否在事务中、已执行语句数） | `Session::StatusLine()` |

**`/api/query` 的响应体**（这是全项目最关键的一个契约）：

```jsonc
{ "ok": true, "data": {
  "session": { "inTxn": false, "txnId": -1 },
  "statements": [{
    "index": 1, "sql": "get id,name in student", "kind": "GET",
    "line": 1, "col": 1,
    "ok": true, "executed": true, "autoCommitted": false,
    "notice": "",
    "elapsedMs": 0.42, "operatorCalls": 3,
    "affected": 0, "tag": "GET 2",
    "columns": [ { "name": "id", "type": "INT" },
                 { "name": "name", "type": "VARCHAR" } ],
    "rows": [ [1, "Alice"], [2, "Bob"] ],
    "truncated": false,
    "error": null,
    "plan": { "before": "...", "after": "..." }
  }]
}}
```

要点：
- **一次请求执行多条语句**，逐条汇报 —— 与 `ScriptReport` 一一对应，编辑器里选中一段也能跑。
- `columns[].type` **引擎没提供**（`ResultColumn` 只有 `name`），因此按「该列首个非 NULL 值的 `Value.type`」推断，
  全为 NULL 时回退 `"NULL"`。这是必须写进测试的行为。
- `rows` 里每个值：NULL → JSON `null`；BOOL → `true/false`；数值 → number；字符串 → string。
  **类型保真**，前端不做字符串化，网格按类型右对齐数字。
- `maxRows` 默认 5000，超出置 `truncated: true` 并在状态栏提示「结果已截断」。
- `error` 与 `compileErrors` 合并：编译期诊断取 `cella_errorText()`，执行期取 `DbStatus::ToString()`，
  两者都带 `code` / `message` / `line` / `col`。

### 4.4 单元测试友好性

`ApiService` 设计成**不依赖 socket**：核心方法接收「路由 + JSON」返回「状态码 + JSON」。
HTTP 服务器只是它的驱动。这样绝大多数测试不需要起监听端口。

---

## 5. 前端设计

### 5.1 布局（信息架构）

```
┌───────────────────────────────────────────────────────────────────────┐
│  cella   [main ▾]  [▶ 执行 Ctrl+Enter] [格式化] │ 事务: ● 空闲 [BEGIN]  │  ← 顶栏 44px
├──────────────┬────────────────────────────────────────────────────────┤
│ 库            │  student  ×  │  查询 1  ×  │  ⚙ 结构  ×               │  ← 标签条 34px
│  ├ ▸ main     ├────────────────────────────────────────────────────────┤
│  │  ├ student │                                                        │
│  │  └ course  │   get id, name, score                                  │  ← 编辑区
│  └ ▸ school   │   in student                                           │     (可拖动分栏)
│     └ grade   │   ordered score desc;                                  │
│               ├────────────────────────────────────────────────────────┤
│               │  id │ name  │ score   ◀ 可排序/可拖列宽/可复制          │  ← 结果网格
│               │  1  │ Alice │  88.5   │                                 │
│               │  2  │ Bob   │  76     │                                 │
├───────────────┴────────────────────────────────────────────────────────┤
│ ✓ 2 行 · 0.42 ms · 缓冲池命中 96.2% · txn -  │ 消息 │ 诊断 │ 计划       │  ← 状态栏 26px
└───────────────────────────────────────────────────────────────────────┘
```

### 5.2 各面板规格

| 面板 | 规格 |
| --- | --- |
| **库/表树** | 两层：库 → 表。库节点右侧显示「当前」徽标；表节点右键菜单：`打开数据` / `查看结构` / `新建查询` / `生成 SELECT` / `删除表` / `复制表名`。支持名称过滤框。点击库节点切换当前库（走 `/api/databases/use`）。 |
| **查询编辑器** | `<textarea>` + 底层 `<pre>` 高亮层的经典叠加方案（不需要 Monaco）。高亮覆盖关键字/字符串/数字/注释/标识符五类。功能：`Ctrl+Enter` 执行（未选中→全部，选中→选区）、`Ctrl+/` 注释、格式化（简单缩进重排）、Tab 缩进、括号自动配对。多标签，每个标签独立保存 SQL 与滚动位置。 |
| **结果网格** | **虚拟滚动**（万行不卡）：只渲染视口内 ±10 行。列头点击排序（升/降/取消三态）、双击列边界自适应宽度、拖拽改列宽、单元格/行/列/全表选择、`Ctrl+C` 复制为 TSV（可直接粘进 Excel）。NULL 显示为灰色斜体 `NULL`；数字右对齐等宽字体。右上角工具条：`导出 CSV` / `导出 JSON` / `复制全部`。 |
| **数据浏览** | 复用结果网格，但增加：分页控件（页码 / 每页 100 / 500 / 1000）、`+ 新增行`、双击单元格内联编辑（乐观提交 + 失败回滚）、行首复选框批量删除。底部显示「第 1–100 行 · 共 ≈1234 行」。 |
| **结构面板** | 表 `#1` · 3 列 · 建表于 2026-09-10 12:33。列清单表格：序号 / 列名 / 类型 / 长度 / 非空。下方折叠区「生成的 DDL」，可直接复制去编辑器改。 |
| **诊断面板** | 四个子页签：缓冲池统计（命中率进度条 + 命中/未命中/淘汰/钉住计数）、锁表（表格：事务 / 表 / 模式 S\|X / 状态）、等待图（边的列表，死锁时高亮环）、事务表（事务号 / 状态 / 持锁数 / 开始时间）。轮询间隔 2 s，可暂停。 |
| **计划视图** | 优化前/后两栏并排，显示为缩进树（把打印出来的计划文本按缩进层级解析）。附「算子调用次数」「耗时」。 |
| **消息区** | 错误列表：每条显示 `[DB-502] 表不存在: student` + 位置 `行 1:8`，点击跳转到编辑器对应位置并选中。 |
| **Toast** | 右下角浮层，`success` / `error` 两种，3 s 自动消失，错误带「查看详情」展开原始信息。 |

### 5.3 视觉规范（「简约、美观」的具体化）

**主题**：CSS 变量 + `<html data-theme="light|dark">`，**默认浅色**（当前 IDE 是浅色），顶栏一键切换并记忆到 `localStorage`。

| 令牌 | 浅色 | 用途 |
| --- | --- | --- |
| `--bg` | `#ffffff` | 主背景 |
| `--bg-sub` | `#f7f8fa` | 树/状态栏/表头底色 |
| `--border` | `#e4e7ec` | 1px 分隔线（唯一的分层手段，不用阴影） |
| `--text` | `#1f2937` | 正文 |
| `--text-dim` | `#6b7280` | 次要信息、NULL |
| `--accent` | `#2563eb` | 主色（按钮、选中、当前库徽标） |
| `--ok` / `--warn` / `--err` | `#16a34a` / `#d97706` / `#dc2626` | 状态语义 |

**排版**：UI 字体 `-apple-system, "Segoe UI", "Microsoft YaHei", sans-serif`；**数据一律等宽**
`"Cascadia Mono", Consolas, monospace`。正文字号 13px，网格行高 28px，顶栏 44px，状态栏 26px，圆角统一 6px。

**克制原则**：不用渐变、不用投影、不用大圆角卡片。层次靠「背景色差 + 1px 边框」，
强调靠「主色 + 字重」。所有数字右对齐、所有可点击元素有 `:hover` 与 `:focus-visible` 态。

**空态**：树为空时给「还没有表，点这里建一张」的引导；结果为空时显示「查询成功，0 行」而不是空白。

### 5.4 技术选型

- **无构建步骤**：原生 ES Module（`<script type="module">`），静态服务器直接发 `.js` 即可。
  改完前端刷新浏览器就生效，不引入 npm / bundler / 转译链。
- **无 UI 框架**：自己写 ~30 行的 `h(tag, attrs, children)` 与一个极简的响应式 store。
  数据量级（几十张表、上万行）不需要虚拟 DOM 框架。
- **依赖：零**。所有资源本地，不发任何外部请求，离线可用。

---

## 6. 关键约束与风险

### 6.1 🔴 行编辑定位：方言没有主键（最高风险）

方言里**没有主键、没有 `rowid`**，`UPDATE` / `DELETE` 只能靠条件：

```sql
UPDATE student SET score = 90 limit id = 2 and name = 'Bob';
DELETE in student limit id = 2 and name = 'Bob';
```

所以前端改一个单元格时，必须把**该行所有列的原始值**拼成 `limit` 条件：

| 情况 | 处理 |
| --- | --- |
| 匹配到 1 行 | 正常提交 |
| 匹配到 0 行 | 报「该行已被其他操作修改或删除，请刷新」并自动重新取数 |
| 匹配到 >1 行 | **拒绝修改**，提示「表中存在完全相同的重复行，无法唯一定位；请改用条件更新」 |
| 列值为 NULL | 生成 `col = NULL` 还是 `col is null`？**必须实测**编译器是否支持 `is null`；不支持则此列不参与定位并在 UI 上提示 |
| 浮点列 | `88.5` 的往返精度：用 `%.17g` 保证文本可还原；若有风险则该列不参与定位 |

> 这是首版必须**先做探针验证**的地方（Phase 0 的一项），因为它的结论会决定数据编辑面板做不做、怎么做。

### 6.2 🟡 没有 COUNT 聚合：总行数要全表扫

`grouped` 在本方言里没有聚合函数，`COUNT(*)` 不存在。所以「共 N 行」只能：

1. 服务端 `get <首列> in <表>`（取一列全量）→ 计数 → 缓存（按表名缓存，任何写操作后失效）；
2. 表大时这一步很慢 → 首次访问**异步**做，UI 先显示「≈ 未知」，拿到再补上；
3. 兜底：如果 `among` 语义允许，用 `among N` 逐级试探上界做近似计数（可选，不在首版）。

### 6.3 🟡 服务端串行 → 长查询期间整个客户端「卡住」

`engine_gate_` 串行是必要的，但一次慢查询会阻塞所有请求。
首版策略：前端全局请求串行 + 明显的加载态 + 禁用「执行」按钮 + 超过 5 s 显示「正在执行，请稍候」。
**不做**查询取消（引擎没有中断点，硬取消会留下未定义状态）。诊断面板轮询在请求进行中自动跳过一轮。

### 6.4 🟡 一个数据目录只能有一个服务进程

`IStorage` 直接打开 `.db` 文件，没有文件锁。两个 `cella_web.exe` 指向同一 `--data` 会互相踩。
对策：启动时在 `data_dir` 写一个 `cella-client.lock`（含 pid + 启动时间），已存在则拒绝启动并提示。

### 6.5 🟡 编辑器高亮的正确性

正则做 SQL 高亮对「字符串里的关键字」「注释里的引号」会出错。首版方案：
手写**单遍扫描的高亮器**（状态机：普通 / 字符串 / 注释），而不是正则替换。
约 80 行，正确性足够，且为将来加括号匹配留了钩子。

### 6.6 🟢 其他

| 风险 | 对策 |
| --- | --- |
| 中文（GBK/UTF-8 混用）乱码 | 全链路 UTF-8：HTTP 头显式 `charset=utf-8`、`<meta charset="utf-8">`、CMake `target_compile_options(... /utf-8)`（沿用 cella_db 的做法） |
| MSVC `/W4 /WX` 在 Winsock 上报 warning | 统一 `#define WIN32_LEAN_AND_MEAN` / `NOMINMAX`；`SOCKET` 类型严格区分；不在头文件里 include winsock（只放 `.cpp`） |
| 端口被占用 | 默认 8080，`--port` 可改；占用时给出明确提示并列出可选端口 |
| 浏览器兼容 | 目标 Chrome/Edge 110+（用到较新的 CSS 与 ES Module）；不做降级 |
| `MAX_PATH` | 数据目录与 web 目录用绝对路径 + `\\?\` 前缀兜底（可选） |

---

## 7. 分期计划

| 阶段 | 内容 | 验收标准（可观察） | 里程碑 |
| --- | --- | --- | --- |
| **P0 骨架 + 探针** | `cella_client/` 模块与 CMake；顶加入 `add_subdirectory`；Socket/HttpServer/Router/Json 最小闭环；`/api/health`；静态资源服务；**方言探针**（`is null` 支持？`limit` 组合项上限？空表列类型？） | `cella_web.exe --data ../mydb` 启动后浏览器打开 `http://127.0.0.1:8080` 看到空白页与健康信息 JSON；探针结论写进本文件 §6.1 | **M0** |
| **P1 SQL 执行链路** | `/api/query` + `/api/plan` + `/api/session` + `/api/txn/*`；前端：顶栏 + 编辑器 + 高亮 + 结果网格（虚拟滚动/排序/复制） + 消息区 + Toast | 在浏览器里粘贴 README 里那段 30 秒示例，能建表、插数据、查出 5 行、错误能定位到行列 | **M1 可用** |
| **P2 目录与数据浏览** | `/api/catalog*` + `/api/tables/*/rows` + `/api/tables/*/count`；前端：库/表树 + 结构面板 + 数据浏览标签（分页/排序） | 点树里的表能看到数据和结构；分页控件与总行数（懒统计）工作正常 | **M2** |
| **P3 数据编辑 + 事务** | `POST/PATCH/DELETE /api/tables/*/rows`；全列匹配行定位策略落地；前端内联编辑 + 新增/删除行 + BEGIN/COMMIT/ROLLBACK 按钮与状态指示 | 能双击改一个单元格并保存；重复行场景给出正确拒绝；事务未提交时状态栏高亮 | **M3** |
| **P4 诊断与打磨** | 四项诊断面板 + 计划视图；深色主题；SQL 历史（localStorage）；导出 CSV/JSON；快捷键；空态/加载态；库的增删切换 UI | 诊断面板能看到缓冲池命中率随查询变化；`\waitfor` 能展示并发演示产生的锁边 | **M4** |
| **P5 收尾** | `cella_client_tests.exe`（HTTP/JSON/路由/端到端）；`cella_client/README.md`；更新顶层 `README.md` 与 `run_all.ps1`；手工验收清单逐条过 | 全量测试通过；`run_all.ps1` 一条命令跑通三层 + 客户端层 | **M5** |

**全程不动的模块**：`cella_sql`、`cella_storage`、`cella_db`（不改一行源码）。
唯一的既有文件改动是顶层 `CMakeLists.txt` 加一行 `add_subdirectory(cella_client)`，
以及 P5 时更新 `README.md` / `run_all.ps1`。

---

## 8. 测试与验收

### 自动化（`cella_client_tests.exe`，沿用 `mini_test.h` 风格）

| 组 | 用例要点 |
| --- | --- |
| HTTP 解析 | 请求行解析、头部大小写、`Content-Length` 边界、畸形请求返回 400、超长头部 413 |
| JSON | 中文往返、转义字符、`double` 精度往返、嵌套对象、`null` 语义、非法输入 |
| 路由 | 路径参数提取、404、405、URL 解码 |
| SQL 生成 | 分页 → `page N, M`、排序 → `ordered`、方向、多列排序、行定位条件的 NULL/浮点分支 |
| 类型映射 | `Value` 各类型 → JSON、全 NULL 列的类型推断、`QueryResult` → `columns/rows` |
| 错误映射 | 编译器诊断（带行列）与 `DbStatus`（不带行列）→ 统一 JSON |
| 端到端 | 起真实监听端口 → socket 发请求 → 断言响应；覆盖建表/插入/查询/改行/删行/事务回滚 |
| 静态资源 | 正常路径、路径穿越（`/../CMakeLists.txt`）被拒、不存在 404 |

### 手工验收清单（写进 `cella_client/README.md`）

- [ ] 数据目录为空时，界面给出建表引导而不是一片空白
- [ ] 10 万行表的滚动帧率可接受（虚拟滚动生效）
- [ ] 中文表名/列名/数据在树、网格、错误消息里都不乱码
- [ ] 浅色/深色主题切换后所有面板可读
- [ ] 长查询期间 UI 不假死、按钮正确禁用、状态栏有提示
- [ ] 关掉服务进程后，前端给出「已断开」提示而不是无限转圈

---

## 9. 对现有工程的改动清单

| 文件 | 改动 | 规模 |
| --- | --- | --- |
| `CMakeLists.txt`（顶层） | 追加 `add_subdirectory(cella_client)`；注释里补一行模块说明 | 1 行 |
| `cella_client/**` | 全部新增 | 大 |
| `README.md`（顶层） | P5：目录结构、构建产物表、快速开始各加一节 | 小 |
| `run_all.ps1` | P5：追加客户端层测试与启动示例 | 小 |
| `.vscode/launch.json` | P5：加 `cella_web` 调试配置（cwd = 仓库根，`--data ./mydb`） | 小 |

**注意**：新增 `cella_client` 目录后，按项目既有约定需要**删除 `build/` 重新 configure**
（`CMakeCache.txt` 里存了源码目录的绝对路径；本次是新增子目录，`CONFIGURE_DEPENDS` 虽然能兜住
源文件变化，但顶层 CMakeLists 变化仍建议重配）。

---

## 10. 待确认事项

| # | 事项 | 需要谁定 | 阻塞关系 |
| --- | --- | --- | --- |
| 1 | 默认端口（建议 `8080`）与默认数据目录（建议仓库根 `./mydb`，与 CLI 示例一致） | 你 | 不阻塞动工，P0 有默认值 |
| 2 | 首版是否要「表设计器」（可视化建表/加列） | 你 | 已按 D3 排除；如果你改主意，P3 之后插入一个 P3.5 |
| 3 | `is null` 语法是否支持 | **探针实测**，见 §6.1 | 阻塞 P3 的行编辑定位策略 |
| 4 | 是否需要 `cella_db` CLI 的 `--serve` 便捷入口（转发到 `cella_web`） | 你 | 不阻塞 |

---

## 附：为什么这一版能真正「实用」

因为它把 cella **独有**的东西做成了界面上的第一等功能，而不是做一个通用的表格工具：

- 执行计划（优化前/后对比）—— 通用工具没有
- 缓冲池命中率、锁表、等待图、事务表 —— 通用工具没有
- 存储层页大小 / 缓冲池帧数 / 替换策略 —— 这是这个项目的教学价值所在
- `cella_catalog` 系统表 —— 天然可作为「系统目录」节点挂进树里，点开就能看到元数据本身

客户端不只是「一个更好看的 CLI」，它是这套系统教学演示的入口。
