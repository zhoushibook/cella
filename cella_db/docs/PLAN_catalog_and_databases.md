# 实现清单：系统目录表化 + 元数据页式持久化 + SQL 级多库

> 目标：一次性关闭对照《指导书》发现的三个缺口。
> 状态：**规划稿，未动工**。按阶段推进，每个阶段独立可验收、可回退。

---

## 0. 目标与验收标准

| 缺口 | 验收标准（可观察） |
| --- | --- |
| ① 系统目录不是「一张特殊的表」 | 存在保留系统表 `cella_catalog`，**能用 SQL 查询**（`get * in cella_catalog`）；建表/删表后目录表内容随之变化 |
| ② 元数据未通过页式存储持久化 | **`catalog.meta` 文件不复存在**；元数据以系统表行的方式存在 `cella.db` 内；数据库恢复为**自包含单文件**；重启后数据不丢（既有测试继续通过） |
| ③ 无 SQL 级多库 | `CREATE DATABASE / DROP DATABASE / USE / SHOW DATABASES` 四条语句可用；库与库之间表完全隔离；每个库是一个自包含的 `.db` 文件 |

配套收益（做完阶段一自动获得）：
- 两份元数据合成一份 → `DB-509` 类不一致**从机制上消失**，`ReconcileCatalogWithStorage` 自愈逻辑可整体删除；
- 目录行写入进入事务 undo，目录变更获得与数据一致的持久化语义；
- 备份/迁移 = 拷一个文件。

---

## 1. 总体顺序（先 1+2，后 3）

```
阶段一  目录表化          ← 关闭缺口 ①②（同一个改动，两个症状）
   ↓  bootstrap 逻辑（打开或创建目录表）在阶段二被逐库复用
阶段二  SQL 级多库         ← 关闭缺口 ③（薄层：库 = <data_dir>/<db>.db 文件）
   ↓
阶段三  收尾：测试 / 文档 / 指导书符合性核对
```

**为什么这个顺序**：先做 3 的话，多库机制会建在 `catalog.meta` 上，阶段一还得把整套多库代码再迁移一遍，做两遍重复功。先做 1+2，阶段二只是「把单库的 bootstrap 和 CatalogManager 逐库实例化」。

**全程不动的模块**：`cella_sql` 编译器（阶段二的 CREATE DATABASE/USE/SHOW 走 Session 层拦截，与 BEGIN/COMMIT 同构）；`cella_storage`（可选加固：`IStorage::ListTables()`，见 §5）。

---

## 2. 阶段一：系统目录表化（关闭缺口 ①②）

### 2.1 设计决策（先定下来再动手）

| 决策点 | 结论 | 理由 |
| --- | --- | --- |
| 系统表名 | `cella_catalog` | 对应 sqlite_master / pg_catalog 的角色；保留名 |
| 表结构 | **单表**：`name VARCHAR(64) NOT NULL, table_id INT NOT NULL, created_at INT NOT NULL, columns TEXT NOT NULL` | 一行一张表、一次扫描即可重建目录；两张表（tables+columns）更"关系型"但要维护两表一致性，教学系统不值得 |
| 列定义编码 | `columns` 列存文本：`id INT NOT NULL,name VARCHAR(32),age INT` | 复用现有 `CatalogTypeName()` 生成、`CatalogTypeFromName()` 解析，两边都是现成函数；标识符字符集安全（字母/数字/下划线），分隔符不会撞 |
| `first_page_id` | **不入库**。加载时对每张表 `open_table()` 顺带补齐（兼做一致性校验） | 它是物理信息，权威在存储层；目录表存了反而可能过期 |
| 保留名保护 | CREATE：把 `cella_catalog` 注册进派生目录 → 语义阶段自然报「表已存在」；DROP/INSERT/UPDATE/DELETE：执行器拦（新码 `DB-512 系统表禁止修改`）；GET：**放行** | 指导书要的就是目录可查；CREATE 走语义阶段零新代码 |
| 旧库迁移 | `Open()` 时若 `catalog.meta` 存在且系统表为空 → 读旧文本 → 写入系统表 → 把旧文件改名 `catalog.meta.migrated` | 用户现有的 `mydb/` 等能直接打开 |
| 墓碑策略 | DROP = 对系统表 `delete_record`（打墓碑），不做压缩 | 教学规模可接受；`Compact()` 留作扩展（§5） |
| DDL 写入时机 | 仍在 `ExecCreateTable/ExecDropTable` 内、持有 `storage_mutex_` 的临界区里写系统表行 | 目录行的写入会进 undo；DDL 整体仍自动提交（页级 create/drop 不可回滚，与现状一致） |

### 2.2 改动文件清单

| 文件 | 改动 | 规模 |
| --- | --- | --- |
| `cella_db/include/cella/db/catalog/catalog_manager.h` | 持久化 API 换血：`Load(dir)/Save()` → `AttachStorage(IStorage*)` + `LoadFromStorage()` + `WriteRow()/DeleteRow()`；新增 `IsSystemTable(name)` | 中 |
| `cella_db/src/cella/db/catalog/catalog_manager.cpp` | **核心**：删文本读写，换成系统表行编解码（列定义文本 ⇄ `CatalogColumn` 列表） | 大（本次最大） |
| `cella_db/src/cella/db/engine/db_engine.cpp` | `Open()`：bootstrap（试探 `open_table("cella_catalog")` → 加载或创建）+ 旧库迁移；**删除** `ReconcileCatalogWithStorage()` 与 `recoveries_`；`main.cpp` 里对应的 `!!` 告警打印一并删 | 中 |
| `cella_db/include/cella/db/engine/db_engine.h` | 同步删成员/声明 | 小 |
| `cella_db/src/cella/db/exec/executor.cpp` | `ExecCreateTable/ExecDropTable`：`catalog_.Save()` → 目录行写入；新增系统表 DROP/DML 拦截（`DB-512`） | 小 |
| `cella_db/src/cella/db/common/db_status.h/.cpp` | 新码 `DB-512`（系统表禁止修改） | 小 |
| `cella_db/src/main.cpp` | 删自愈告警打印；`\d` 可顺带显示目录表行数 | 小 |
| `cella_db/tests/test_catalog.cpp` | **重写大半**：文本文件相关用例（文件头校验/原子改名/落盘重载）换成系统表用例 | 中 |
| `cella_db/tests/test_txn.cpp`、`test_e2e.cpp` | 自愈相关用例删除/改写；新增目录表用例 | 小 |
| 文档 ×4 | `INTEGRATION.md`（§2.4 接缝④、§5 改动清单、§6 取舍 8/8b/11）、`ARCHITECTURE.md`（目录/4.8）、`README.md`、`TEST_REPORT.md` | 小 |

`cella_sql`、`cella_storage`：**零改动**。

### 2.3 实施步骤（按序勾选）

1. [x] `CatalogManager`：定义系统表 schema 常量（`kCatalogTableName = "cella_catalog"` + 列定义），加 `AttachStorage()`
2. [x] 实现「列定义文本 ⇄ CatalogColumn」双向编解码（复用 `CatalogTypeName/CatalogTypeFromName`），含单元测试先行
3. [x] 实现 `LoadFromStorage()`：全表扫描（`TableHeap::begin/end`，跳过墓碑）→ 解析每行 → 重建 `tables_` → 逐表 `open_table()` 补 `first_page_id`
4. [x] 实现 `WriteRow()/DeleteRow()`：insert/delete 系统表行（供执行器 DDL 调用）
5. [x] `DbEngine::Open()`：bootstrap 三分支（有系统表→加载；无→创建；其他存储错误→报错）+ 旧 `catalog.meta` 一次性迁移
6. [x] 执行器：`ExecCreateTable/ExecDropTable` 改调目录行写入；删掉 `catalog_.Save()` 调用
7. [x] 保留名拦截：CREATE 走语义（系统表进派生目录）、DROP/DML 走执行器（`DB-512`）、GET 放行
8. [x] 删除 `ReconcileCatalogWithStorage` / `recoveries_` / `RecoveryReport` 及 main.cpp 告警打印
9. [x] 重写/调整受影响测试（见 2.4）
10. [x] 全量 `run_all.ps1` 回归 + 文档更新

### 2.4 测试清单（新增/改写）

| 用例 | 断言 |
| --- | --- |
| `目录_系统表可查询` | `get * in cella_catalog` 返回建过的表，列数/类型/NOT NULL 解码正确 |
| `目录_保留名写保护` | `CREATE TABLE cella_catalog(...)` → SEM 表已存在；`DROP TABLE cella_catalog` / 对它 INSERT / DELETE → `DB-512` |
| `目录_元数据随DDL变化` | CREATE 后目录表多一行；DROP 后该行打墓碑、查不到 |
| `目录_重启持久化` | 关闭重开（干净退出）→ 表结构完整；删掉 `cella.db` → 报存储层错误（**不再有** DB-509 与自愈） |
| `目录_单文件自包含` | 只拷 `cella.db` 到新目录即可打开（不再需要 catalog.meta） |
| `目录_旧库迁移` | 用旧版造一个带 `catalog.meta` 的目录 → 新版打开 → 表都在、`catalog.meta.migrated` 存在 |
| `目录_列定义往返` | 全类型（9 种）+ 长度 + NOT NULL 的表，重启后结构逐字段一致 |
| `执行_DDL立即持久化` | 保留（语义不变，现在刷的是目录表页） |
| 删除 | `事务_目录与数据文件不一致时自愈`（机制已不存在） |

### 2.4b 阶段一完成记录（2026-09-11）

* 全部步骤完成；回归：编译器 29/29、存储层 30/30（4620 断言）、整合层 **75/75（616 断言）** 全绿。
* 实施中的关键修正：旧库迁移除了写目录行，还必须 `EnsurePhysicalTable()` 重建缺失的物理表
  （旧目录可能只带 catalog.meta 文本），否则 `LoadFromStorage` 会把无物理背板的条目剔除。
* 存储层、编译器零改动（与本清单承诺一致）。

### 2.5 风险与对策

| 风险 | 对策 |
| --- | --- |
| 单行过长：列极多的表，`columns` 文本可能超页（DB-510） | 教学规模（几十列 ≈ 1KB）远低于 4KB；清单注明边界；真要支持再做"每列一行"的两表方案 |
| 目录行写入的加锁 | 必须发生在持有 `storage_mutex_` 的临界区内——执行器 DDL 路径天然满足，`CatalogManager` 注释里写死这条约定 |
| 既有 `test_catalog.cpp` 大半作废 | 视为计划内重构，先写新用例再删旧用例，避免中间态红 |
| 旧库迁移失败导致打不开 | 迁移包 try：失败则报清晰错误并保留 `catalog.meta` 原文件，不删不覆写 |
| 目录页单页容量（存储层既有限制） | 与本次无关，但"大量表"场景会先撞它；文档注明 |

---

## 3. 阶段二：SQL 级多库（关闭缺口 ③）

### 3.1 设计决策

| 决策点 | 结论 | 理由 |
| --- | --- | --- |
| 库的物理形态 | **一个库 = `<data_dir>/<db>.db` 一个自包含文件** | 阶段一之后每库只需一个文件；备份=拷文件；`SHOW DATABASES`=列目录文件，无需注册表 |
| 默认库 | `main`（`main.db`）；`--db` 参数继续可用（指定启动库） | 兼容 CLI 习惯 |
| 语句实现路线 | **Session 层拦截**（`IsDatabaseControl()`，与 BEGIN/COMMIT 同构），编译器零改动；覆盖 `CREATE DATABASE / DROP DATABASE / USE / SHOW DATABASES` 四条 | 见 3.1.1；这些语句不产生数据计划，走编译器纯属浪费 |
| 语句识别 | 匹配**前两个词**：`CREATE|DROP` + `DATABASE`，以及 `USE`、`SHOW DATABASES`；`CREATE TABLE` 不受影响；容忍前置注释 | 只看首词会把 `CREATE TABLE` 误吞 |
| 「当前库」归属 | **引擎级**（全实例共享一个当前库），文档明示 | 现架构 `storage_/catalog_` 本就是引擎级单例；会话级当前库需要每会话一套 storage+catalog，重构不成比例。跨库并发用多个引擎实例（多个进程）——这正是现在 `--data` 的模型 |
| `USE` / `DROP` 与活动事务 | 库级写操作（CREATE/DROP/USE）在事务中一律报 `DB-513 数据库操作前须结束当前事务`；`SHOW DATABASES` 随时可用 | undo 里记录的是旧库的表名，切换会悬空；一条规则覆盖全部库级语句，好测好记 |
| `DROP DATABASE` | **软删除**：把 `<db>.db` 改名 `<db>.db.dropped-<时间戳>`，不物理删除。拦截：目标是当前库（DB-515）、目标是 main（DB-515）、库不存在（DB-514） | 删库不可逆，改名可手动恢复；当前库的 storage 正开着，物理删会炸；main 是默认落点，误删等于自断退路 |
| 锁资源名 | 从 `表名` 改为 `<db>.<table>` | 不改的话，两个库里同名表会共享同一把锁 → 假冲突/假死锁。**必须做** |
| 日志 | `journal.log` 保留实例级，行内加库名前缀；`cella-db.log` 不变 | 少动；也免去删库时清理日志文件的麻烦 |

#### 3.1.1 为什么编译器可以零改动（对「编译器缺库语法」的正面回答）

编译器"缺"这三条语法**不影响它们可用**，因为这三条语句**根本不进编译器**。`Session::ExecuteOne` 里已经有一个先例：`BEGIN / COMMIT / ROLLBACK` 也不在编译器文法里（编译器见到同样报 SYN-201），但它们在进入编译器**之前**就被 `IsTxnControl()` 拦下、由会话直接执行了。数据库控制语句走完全相同的位置。

三个理由：

1. **职责归属**：指导书给编译器划的范围是四类核心语句的词法/语法/语义/计划；建库、切库是**引擎/会话层**的能力，本来就不是编译器的活。
2. **没有计划可生成**：这三条语句不读不写任何表数据，计划器对它们无事可做——为它们造计划节点是纯仪式成本。
3. **golden 契约风险**：碰文法就要碰 `tests/expected/*.txt`（29 个回归用例是硬约束），而收益接近零。

走编译器路线的真实代价（如果将来想这么做）：

| 代价 | 说明 |
| --- | --- |
| 新增保留字 | `DATABASE/USE/SHOW` 进关键字表后，用户从此不能建名叫 `show`/`use`/`database` 的表或列——纯粹的兼容性损失 |
| 动文法 | 语法/AST/计划/打印四处都要加节点，而 golden 契约盯着这个模块 |
| 输出不对称 | 还得决定 `--all` 四阶段里它们输出什么 |

所以推荐拦截路线，但要在拦截层自己保证两件事：**语句识别**（前两词匹配，别误吞 `CREATE TABLE`）和**诊断格式统一**（错误复用 `CELLA_Error` 构造，输出形态与编译器诊断一致）。这两条已列入 §3.3 步骤 1。

### 3.2 改动文件清单

| 文件 | 改动 | 规模 |
| --- | --- | --- |
| `cella_db/include/cella/db/engine/db_engine.h` + `.cpp` | `UseDatabase(name)` / `CreateDatabase(name)` / `DropDatabase(name)` / `ShowDatabases()`；`Open()` 兼容旧布局迁移（根下散落 `cella.db` → `main.db`）；`USE` 时先 Checkpoint 再换 `storage_config_.db_file` 并重开 | 中 |
| `cella_db/src/cella/db/engine/sql_text.cpp` + `db_engine.h` | `IsDatabaseControl()`（识别 `CREATE|DROP DATABASE` / `USE` / `SHOW DATABASES`，**前两词匹配**，容忍前置注释）+ 库名合法性校验 | 小 |
| `cella_db/src/cella/db/engine/db_engine.cpp`（Session::ExecuteOne） | 事务控制拦截分支旁新增数据库控制分支；产出 `StatementOutcome`（`kind` = CREATE DATABASE / DROP DATABASE / USE / SHOW DATABASES） | 小 |
| `cella_db/src/cella/db/common/db_status.h/.cpp` | `DB-513`（数据库操作前须结束事务）、`DB-514`（数据库已存在/不存在）、`DB-515`（目标库不可删除：当前库或默认库 main） | 小 |
| `cella_db/src/cella/db/exec/executor.cpp` | 锁资源名 → `<db>.<table>`（`ExecCreateTable/ExecDropTable/ExecInsert/ExecDelete/ExecGet` 里所有 `LockTable` 调用点） | 中（机械但分散） |
| `cella_db/src/cella/db/txn/transaction.h/.cpp` | undo 记录的表名同步加库前缀（与锁一致） | 小 |
| `cella_db/src/main.cpp` | 提示符显示当前库（`cella(main)>`）；`\l` 列库；`--db` 帮助文本更新 | 小 |
| 测试 | 新增 `test_database.cpp`：多库隔离/切换/持久化/锁不串库 | 中 |
| 文档 ×4 | 同阶段一 | 小 |

### 3.3 实施步骤（按序勾选）

1. [ ] `IsDatabaseControl()`（**前两词匹配**，别误吞 `CREATE TABLE`）+ 四条语句的 Session 层解析 + 库名合法性校验（标识符规则、保留字拒绝）
2. [ ] `CreateDatabase`：`<data_dir>/<name>.db` 不存在 → 初始化（复用 `Open()` 的存储引导路径）；已存在 → `DB-514`
3. [ ] `DropDatabase`：库不存在 → `DB-514`；目标是当前库或 main → `DB-515`；否则把 `<db>.db` 改名 `<db>.db.dropped-<时间戳>`（软删除，可手动恢复）
4. [ ] `ShowDatabases`：列 `<data_dir>/*.db`（`main` 恒在），返回 `QueryResult`（单列 name）
5. [ ] `UseDatabase`：库存在性检查 → 事务中拦截（DB-513）→ `Checkpoint()` 当前库 → 换 `db_file` 重开 storage → `LoadFromStorage()` → 更新会话当前库名
6. [ ] 锁与 undo 的资源名加库前缀（全量过一遍 `LockTable` 调用点 + undo 记录）
7. [ ] CLI：提示符/`\l`/帮助
8. [ ] 旧布局迁移（根下散落 `cella.db` → `main.db`，提示一次）
9. [ ] 测试与文档

### 3.4 测试清单

| 用例 | 断言 |
| --- | --- |
| `多库_创建与列举` | CREATE DATABASE 后 `SHOW DATABASES` 能看到；重复创建 → `DB-514` |
| `多库_切换与隔离` | 库 A 建表 t 并插入；`USE b` 后 `get * in t` → 表不存在；切回 A 数据还在 |
| `多库_每库自包含` | 只拷 `b.db` 到别处即可作为独立库打开 |
| `多库_锁不串库` | 两库各有同名表 t：A 库持 t 的 X 锁时，B 库写 t **不被阻塞**（锁资源名含库前缀） |
| `多库_事务中禁止库级操作` | BEGIN 后 `USE x` / `CREATE DATABASE y` / `DROP DATABASE z` → `DB-513`，事务仍可用；`SHOW DATABASES` 可用 |
| `多库_删除_正常路径` | `DROP DATABASE b` 后 `b.db` 改名为 `b.db.dropped-<ts>`（文件还在）；`SHOW DATABASES` 不再列出；`USE b` → `DB-514` |
| `多库_删除_拦截` | 删当前库 → `DB-515`；删 main → `DB-515`；删不存在的库 → `DB-514` |
| `多库_删除_可恢复` | 把 `.dropped-<ts>` 文件改回 `<db>.db` 后可正常打开，数据完整 |
| `多库_非法名` | `CREATE DATABASE`（缺名）/ 保留字 / 非法字符 → 编译错误 |
| `多库_默认库` | 不 USE 时一切照旧落在 main；`--db` 指定启动库 |
| `多库_重启回到当前库` | 关闭前在库 b，重启（记录当前库）后仍在 b —— 或明确文档化为「重启回 main」，二选一写清 |
| `多库_误吞检查` | `CREATE TABLE database(x INT);` 等含 "database" 字样的普通语句不被拦截 |

---

## 4. 阶段三：收尾

1. [ ] 全量 `run_all.ps1` 回归（编译器 29 / 存储层 30 / 整合层全部用例）
2. [ ] 指导书符合性核对：三条缺口逐条对照原文关闭（目录表 / 页式持久化 / 多库）
3. [ ] 文档四处同步（INTEGRATION / ARCHITECTURE / README / TEST_REPORT），并在 INTEGRATION §5 记录本次改动清单
4. [ ] git 提交拆分：阶段一一个 commit，阶段二一个 commit（便于回退）

## 5. 明确不做 / 留待扩展

| 项 | 为什么不做 |
| --- | --- |
| 跨库查询（`db.table` 语法） | 需要编译器支持两段式表名，超出本次范围 |
| `CatalogManager::Compact()`（系统表墓碑压缩） | 教学规模下墓碑可接受；实现要保证 table_id 稳定 |
| `IStorage::ListTables()` | 零改动方案可行；加了能检测孤儿表、bootstrap 更稳，留作独立加固 |
| 系统表的行级权限/视图 | 明显超纲 |
