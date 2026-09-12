# cella_client —— 图形客户端

用浏览器操作 cella 数据库的图形客户端：**内嵌 HTTP 服务（Winsock2，零第三方依赖）+
浏览器单页应用（无构建步骤）**。形态与信息密度对标 Navicat 的日常使用子集，
并把 cella 独有的能力（执行计划、缓冲池/锁/等待图诊断、rowid 伪列）做成了界面上的第一等功能。

设计与实现约定见 [docs/PLAN_web_client.md](docs/PLAN_web_client.md)。

---

## 1. 快速开始

```powershell
# 构建后启动（数据目录/端口按需调整）
.\build\cella_client\cella_web.exe --data .\mydb --port 8080
# 浏览器打开 http://127.0.0.1:8080
```

只监听 `127.0.0.1`，不对局域网暴露。`--help` 查看全部参数。

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--data DIR` | `./cella_data` | 数据目录（与 cella_db CLI 同义） |
| `--db NAME` | `main` | 启动库 |
| `--port N` | `8080` | 监听端口 |
| `--web-dir DIR` | 编译期注入 | 前端资源目录（开发前端时指向工作副本） |
| `--page-size` / `--pool` / `--replacer` | 4096 / 64 / LRU | 存储层参数，与 CLI 一致 |

> **同一数据目录只能有一个服务进程**（`data_dir` 下的 `cella-client.lock` 兜底，
> 因为 `IStorage` 没有文件锁）。

---

## 2. 界面导览

| 区域 | 内容 |
| --- | --- |
| 顶栏 | 库切换（+ 新建 / − 删除库）、执行（`Ctrl+Enter`）、格式化、导出、事务三件套、主题切换 |
| 左侧树 | 当前库的表清单（含系统表 `cella_catalog`），名称过滤；右键表名：打开数据 / 查看结构 / 新建查询 / 复制表名 / 删除表 |
| 查询标签 | SQL 编辑器（方言高亮、自动缩进、`Ctrl+Enter` 执行——选中片段只跑选区）、结果网格（虚拟滚动、`Ctrl+C` 复制为 TSV）、消息区（点击错误跳转到对应行列） |
| 数据标签 | 表数据浏览：小表（≤2 万行）一次性全量加载 + 本地翻页/排序；大表服务端分页 + 懒统计总行数。双击单元格内联编辑、右键删行、`+ 新增行` |
| 结构标签 | 列清单（类型/长度/非空/主键标记）+ 还原的建表 DDL |
| 底部面板 | 消息 / 执行计划（优化前后并排）/ 诊断（缓冲池、锁表、等待图、事务表，2s 自动刷新可暂停） |
| 状态栏 | 数据目录与引擎参数、行数与耗时、事务状态 |

深色主题：右上角 ◐ 切换，记忆在 `localStorage`。

---

## 3. 使用须知（方言与引擎的边界）

- **方言**：`get`=SELECT、`in`=FROM、`limit`=WHERE、`ordered`=ORDER BY、`among`=LIMIT 行数、
  `page 页码, 每页行数` 原生分页（页码从 1 起）。完整对照见 `cella_db/README.md` §3。
- **NULL 判断必须写 `is null`**（`x = NULL` 永不成立）。
- **编辑定位（三级矩阵）**：有主键的表按主键定位（最安全）；无主键的表用 `rowid` 伪列定位；
  保存时服务端在事务内**重取该行与界面旧值比对**（乐观校验），不一致则拒绝并刷新。
- **`UPDATE` 会把该行物理移到表尾**（删旧+插新），rowid 随之变化 —— 所以数据浏览默认
  「有主键按主键排序，无主键按 rowid 排序」，保存后自动重取当前页。
- **无主键表里全列相同的重复行**：全列匹配无法区分，靠 `rowid` 才能精确改/删其中一行 ——
  这正是数据编辑面板在无主键表上仍然安全的原因。
- **删除/重命名库表**：`DROP TABLE` 可用；清空表用 `delete in 表;`（无条件 DELETE 合法）；
  `ALTER TABLE` / `TRUNCATE` / `RENAME` **引擎未实现** → 无法给已有表加列/改列/改名（只能重建）。
- **总行数**：方言没有 COUNT 聚合。小表全量加载后总数即已知；大表显示「≈?」并懒统计。

---

## 4. HTTP API（前端契约，可独立使用）

统一外形：成功 `{ok:true, data:{...}}`；失败 `{ok:false, error:{code,message,line,col,detail}}`。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/health` | 引擎状态与配置 |
| `GET/POST` | `/api/databases[...]` | 库列表 / use / create / drop |
| `POST` | `/api/query` | 执行 SQL（多条语句逐条汇报，含结果集/计划/错误） |
| `POST` | `/api/plan` | 只编译返回优化前后计划 |
| `GET` | `/api/session` | 会话状态（事务号等） |
| `POST` | `/api/txn/{begin\|commit\|rollback}` | 事务控制 |
| `POST` | `/api/checkpoint` | 存盘点 |
| `GET` | `/api/diagnostics/{stats\|locks\|waitfor\|txn}` | 诊断文本 |
| `GET` | `/api/catalog`、`/api/catalog/{table}` | 表目录（含 `primaryKey.columns` 数组） |
| `GET` | `/api/tables/{t}/rows` | 分页取数（固定含 `rowid` 列） |
| `GET` | `/api/tables/{t}/count` | 行数（全表扫，懒统计用） |
| `POST/PATCH/DELETE` | `/api/tables/{t}/rows` | 新增 / 修改 / 删除一行（`key` 定位键 + `expect` 乐观校验） |

错误码沿用引擎分段（`LEX/SYN/SEM/PLN` 与 `DB-5xx/6xx/7xx`）；诊断行列是**语句内坐标**，
`error.absLine` 已换算为脚本绝对行号，前端直接用于跳转。

---

## 5. 架构与测试

```
浏览器（原生 ES Module，零依赖零构建）
   │ HTTP/1.1 + JSON（127.0.0.1）
cella_web.exe
   ├─ net/     Socket(RAII) · HttpParser · HttpServer(每连接一线程+keep-alive) · StaticFiles
   ├─ api/     Json(自研) · Router · SqlBuilder(方言生成) · ErrorMap(坐标换算)
   └─ server/  ApiService（全部 REST；engine_gate_ 串行化，锁序 gate_ → storage_mutex_）
   └─ 进程内直调 cella_db_core（DbEngine → Session → Executor → IStorage，零改动）
```

- `ApiService` 不依赖 socket：测试直接构造 `HttpRequest` 调 `Handle()`，绝大多数用例无需起端口。
- 测试：`build/cella_client/cella_client_tests.exe`（JSON 往返与容错 / HTTP 解析与畸形请求 /
  SQL 生成 / API 端到端 / 坐标换算黄金样本）。`run_all.ps1` 已纳入。

---

## 6. 已知限制

- 不支持 HTTPS / 局域网访问（设计如此：本机工具）。
- 一次一个数据目录、一个库连接（多库切换 = `USE`，共享同一引擎）。
- 服务端串行执行：长查询期间整个客户端等待（前端有加载态；不支持取消）。
- 表设计器：受 `ALTER TABLE` 未实现所限，只能建新表，不能改已有表结构。
- SQL 历史记录（localStorage）尚未实现。
