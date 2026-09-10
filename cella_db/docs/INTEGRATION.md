# 模块整合说明：编译器 ⇄ 操作系统（存储）⇄ 数据库层

> 本文回答本次整合的第一个要求：**梳理编译器与操作系统两部分之间的接口与依赖关系**，
> 说明编译器输出的指令如何被操作系统模块调度与执行，以及新增数据库层与之如何协作。
>
> 阅读顺序建议：§1 全景 → §2 接口清单 → §3 数据流 → §4 内存管理与调度 → §5 改动与取舍。

---

## 1. 全景：三个模块、四道接缝

整合后的系统由三个可独立构建的模块组成，依赖关系**严格单向、无环**：

```
                    ┌─────────────────────────────┐
   SQL 文本 ───────▶ │  cella_sql（编译器前端）      │
                    │  lexer → parser → semantic   │
                    │  → planner → optimizer       │
                    └──────────────┬──────────────┘
                      执行计划（可执行 IR）
                                   │  接缝①：计划节点
                    ┌──────────────▼──────────────┐
                    │  cella_db（本次新增）         │
                    │  Session / DbEngine          │
                    │  Executor  ← 计划驱动执行     │
                    │  CatalogManager（元数据）     │
                    │  LockManager + TxnManager    │
                    └──────────────┬──────────────┘
                       表级接口调用 │  接缝②：IStorage
                    ┌──────────────▼──────────────┐
                    │  cella_storage（操作系统侧）  │
                    │  页式存储 + 缓冲池（内存管理）│
                    │  替换策略 + 磁盘后端          │
                    └─────────────────────────────┘
```

另外两道接缝是**横向的类型/元数据适配**：

| 接缝 | 位置 | 作用 |
| --- | --- | --- |
| ① 计划节点 | `cella_planner.h` → `cella_db/exec/executor.*` | 编译器把「做什么」交给执行器 |
| ② 存储接口 | `i_storage.h` → `cella_db/exec/executor.*` | 执行器把「读写哪些行」交给存储层 |
| ③ 类型桥 | `cella_db/common/value_bridge.*` | SQL 类型/字面量 ⇄ 存储 `Value` |
| ④ 目录桥 | `cella_db/catalog/catalog_manager.*` | 数据库目录 ⇄ 编译器目录 ⇄ 存储 Schema |

**依赖规则（可机械检查）**

| 模块 | 允许依赖 | 实际依赖 |
| --- | --- | --- |
| `cella_sql` | 仅 C++17 标准库 | ✅ 不包含任何 `cella/storage/...` 头 |
| `cella_storage` | 仅 C++17 标准库 | ✅ 不包含任何 `cella/cella_*.h` 头 |
| `cella_db` | 上述两者 | ✅ 唯一把它们连起来的模块 |

因此「编译器不认识存储、存储不认识 SQL」这一良好性质在整合后依然成立：
耦合被集中到 `cella_db` 一个模块内，而不是散落到两边。

---

## 2. 接口清单（逐条给出契约）

### 2.1 接缝①：编译器 → 执行器（计划节点）

编译器对外的结构化产物有三级：`Token[]` → `CELLA_Program`(AST) → `CELLA_PlanNode[]`(计划)。
整合层**以计划树为主干驱动执行**，AST 作为类型化信息源按需回指。

`cella_sql_core` 导出（公开头文件）：

```cpp
std::vector<CELLA_Token>  cella_tokenize(const std::string& sql, std::vector<CELLA_Error>&);
std::unique_ptr<CELLA_Program> cella_parse(const std::vector<CELLA_Token>&, std::vector<CELLA_Error>&);
CELLA_SemanticResult cella_analyze(const CELLA_Program&, CELLA_Catalog&);
std::vector<std::unique_ptr<CELLA_PlanNode>> cella_plan(const CELLA_Program&, const std::vector<bool>& stmtOk,
                                                        const std::vector<std::vector<std::string>>& insertCols,
                                                        std::vector<CELLA_Error>&);
CELLA_OptResult cella_optimizePlans(const std::vector<std::unique_ptr<CELLA_PlanNode>>&);
```

**为执行而附加的字段**（`CELLA_PlanNode`，纯增量、不改动原字段语义）：

| 字段 | 用于哪些算子 | 为什么打印文本里不够用 |
| --- | --- | --- |
| `const CELLA_Stmt* stmt` | CreateTable / Insert / Delete / Update / DropTable | DDL/DML 需要列定义、插入值、SET 赋值等**全量类型化**信息，`extra` 里是给人看的字符串 |
| `const CELLA_TableRef* tableRef` | SeqScan | 需要表名与**别名**（行上下文的限定名） |
| `exprs` + `selectAliases` | Project | 投影表达式树；`detail` 只有渲染后的文本 |
| `sortKeys` + `sortAsc` | Sort | 排序列与方向的结构化表示 |
| `groupKeys` | Aggregate | GROUP BY 键 |
| `rowLimit` / `pageOffset` / `pageSize` | Limit / Page | 行数上限与分页偏移 |

三条约束保证这次扩展是安全的：

1. **打印零变化**：`cella_printer.cpp` 完全不读取这些字段 ⇒ 既有 golden 文本逐字节不变。
2. **优化器不丢信息**：`cella_optimizer.cpp::cloneShell` 逐字段深拷贝（表达式用 `cella_cloneExpr`），
   所以常量折叠后的计划仍可执行。
3. **指针生命周期明确**：`stmt` / `tableRef` 指向 `CELLA_Program` 内部对象，
   执行期间由调用方（`Session`）保证 AST 存活。

> 为什么不让 DDL/DML 也「从计划文本反解析」？因为那要把给人看的字符串再解析成值，
> 既脆弱又重复劳动。计划树承载结构（算子、子节点、谓词、排序键），
> AST 承载类型化载荷（列定义、字面量、赋值），这条分工是刻意的。

### 2.2 接缝②：执行器 → 存储（IStorage）

`cella_storage` 对外的唯一契约面是 `IStorage`（页级 + 表级）与 `common/` 公共类型。
整合层**只使用表级接口**，页级接口保留给存储层内部与将来的索引/溢出扩展。

| 计划算子 | 执行动作 | 调用的存储接口 | 需要的锁 |
| --- | --- | --- | --- |
| `CreateTable` | 登记目录 + 建物理表 + 取首数据页 | `create_table` → `open_table` | X(table) |
| `DropTable` | 释放整条页链 + 删目录项 | `drop_table` | X(table) |
| `Insert` | 求值 → 约束校验 → 组装 Record | `insert_record` | X(table) |
| `Delete` | 谓词扫描 → 逐行打墓碑 | `open_table` + `TableHeap::begin/end` + `delete_record` | X(table) |
| `Update` | 谓词扫描 → 删旧 + 插新 | `delete_record` + `insert_record` | X(table) |
| `SeqScan` | 全表扫描（跳过墓碑） | `open_table` + `TableHeap::begin/end` | S(table) |
| `Filter` / `Project` / `Aggregate` / `Distinct` | 纯内存运算 | — | 继承子算子 |
| `Join` | 嵌套循环连接（middle / left / right） | — | 继承两侧 |
| `Sort` / `Limit` / `Page` / `Union` | 纯内存运算 | — | 继承子算子 |

两个刻意的设计点：

* **Filter/Project 不下推到存储层**。存储层只提供原始全表扫描——这正是
  `cella_storage/include/cella/storage/integration/plan_bridge.h` 里写明给「引擎组」的约定，
  本层就是那个引擎组。谓词过滤、投影、Top-N 全在内存内完成。
* **锁在扫描/写表前申请，而不是在语句入口统一申请**。这样 JOIN 的两张表按计划顺序加锁，
  `get` 只加 S 锁，DML 只加目标表的 X 锁；也天然避免了「先拿存储互斥量、再等数据库锁」的交叉等待。

### 2.3 接缝③：类型桥（value_bridge）

| SQL 类型 | 存储物理类型 | 说明 |
| --- | --- | --- |
| `INT` | `kInt32` | 一对一 |
| `FLOAT` / `DOUBLE` | `kFloat` / `kDouble` | 一对一 |
| `CHAR` / `VARCHAR` / `TEXT` | `kVarchar` | 存储层只区分定长/变长；长度约束在本层校验 |
| `DATE` / `TIME` / `DATETIME` | `kVarchar` | **存储层序列化器未实现 `kDate` 物理类型**，按 ISO 文本存储；ISO 文本的字典序与时间序一致，比较语义正确 |

取值转换（`CoerceValue`）是执行期的最后一道类型闸门：

| 输入 → 目标 | 结果 |
| --- | --- |
| 数值 → `INT` | 值为整（`1.0`）则收；有小数（`1.5`）报 **DB-505** |
| 数值 → `FLOAT`/`DOUBLE` | 直接提升 |
| 字符串/日期 → 字符族 | 校验声明长度，超长报 **DB-508** |
| 数值 ↔ 字符串 | 跨族，报 **DB-505** |
| `NULL` → `NOT NULL` 列 | 报 **DB-506** |

> 编译器的语义阶段用的是**简化类型兼容规则**（`NUMBER` 可进 `INT/FLOAT/DOUBLE`），
> 所以 `INSERT ... VALUES (1.5)` 到 `INT` 列会通过编译、在执行期被拒。
> 这不是漏洞而是分工：编译期管「能不能表达」，执行期管「值能不能落库」。

### 2.4 接缝④：目录桥（三份元数据的一致性）

系统中存在三份「表结构」表示，方向固定、以数据库层为权威：

```
storage 内部最小目录 ──load_directory()──▶ 仅服务页/表堆，IStorage 不导出枚举接口
        ▲
        │ create_table / drop_table（DDL 时同步）
        │
CatalogManager（权威）──ToCompilerCatalog()──▶ cella::CELLA_Catalog（语义分析的输入）
        │
        └──ToStorageSchema()──▶ storage::Schema（建表时下发给存储层）
```

* 启动时 `CatalogManager::Load` 读 `<data_dir>/catalog.meta`；
  随后对每张表调用 `IStorage::open_table` **互相校验**，缺失即报 **DB-509**（目录与数据文件不一致）。
* 每条语句执行完毕后 `Session` 用 `CatalogManager::ToCompilerCatalog()` 重建
  `cella::CELLA_Catalog`，因此 `CREATE TABLE` 对后续语句立即可见，且语义阶段的目录突变
  不会污染权威目录。
* 目录文件写入采用「临时文件 + 原子改名」，格式为带版本头的行式文本（`CELLA-CATALOG 1`）。

---

## 3. 端到端数据流

以 `get s.name, c.title in student s middle join course c on s.cid = c.id ordered s.name asc;` 为例：

```
① 文本切分      SplitSqlStatements()            逐条语句（认字符串/注释边界）
② 词法           cella_tokenize                 Token[]，位置精确到 行:列
③ 语法           cella_parse                    CELLA_Program（AST）
④ 语义           cella_analyze(compiler_catalog) 表/列/类型检查；OK 或 SEM-3xx
⑤ 计划           cella_plan                     Sort(Project(Join(SeqScan, SeqScan)))
⑥ 优化           cella_optimizePlans            常量折叠 / 布尔化简 / 恒真 Filter 消除
⑦ 事务           TxnManager::Begin              显式事务或自动提交事务
⑧ 加锁           LockManager::Acquire           S(student) + S(course)
⑨ 执行           Executor::Run 递归调度算子
                 ├─ SeqScan(student)  → IStorage::open_table + TableHeap 迭代
                 ├─ SeqScan(course)   → 同上
                 ├─ Join              → 嵌套循环 + ON 谓词求值（内存）
                 ├─ Project           → 表达式求值（含隐藏排序列）
                 └─ Sort              → 排序后裁掉隐藏列
⑩ 结果           QueryResult{columns, rows, tag} + 耗时/算子次数/事务号
⑪ 收尾           Commit（自动提交）或按错误策略回滚
⑫ 日志           cella-db.log（分层日志）+ journal.log（提交/回滚审计）
```

**错误策略**（`Session::ExecuteOne`）：

| 失败原因 | 处理 |
| --- | --- |
| 编译期错误（LEX/SYN/SEM/PLN） | 语句不执行；目录复位；**不影响后续语句**（沿用编译器原语义） |
| 运行期错误 + 自动提交模式 | 回滚整个自动提交事务（本语句改动全部撤销） |
| 运行期错误 + 显式事务 | **语句级回滚**：只撤本语句的 undo，事务保持可用（MySQL 风格） |
| 死锁 / 锁超时（`DB-604`/`DB-605`） | 本事务是牺牲者 → **整事务回滚**并释放锁 |

---

## 4. 与操作系统机制的衔接

### 4.1 内存管理 ← 缓冲池

存储层内部就是一套完整的内存管理：固定帧数的缓冲池（`pool_size`）、
LRU/FIFO/CLOCK 替换策略、pin/unpin 计数、脏页回写、页级 CRC 校验开关。

整合层的定位是**内存管理策略的使用者与观察者，而不是实现者**：

| 关注点 | 由谁负责 | 数据库层做什么 |
| --- | --- | --- |
| 页的换入换出 | 存储层缓冲池 | 不感知；只调用表级接口 |
| 缓冲池容量/策略 | `EngineConfig.pool_size` / `replacer` | 通过 CLI `--pool` / `--replacer` 透传 |
| 内存不足 | 存储层淘汰页 | 无需处理；由淘汰策略保证不 OOM |
| 统计与可观测 | `BufferStats` | `--stats` / `\stats` / `DbEngine::StatsText()` 暴露命中率、淘汰数、钉住页 |

CLI 演示：`cella_db --pool 4 --replacer CLOCK --stats ...` 可在 5000 行表上观察到
大量淘汰（`evictions` 增长）而系统仍正确。

### 4.2 任务调度 ← 三层调度

「调度」在整合系统里分三层，职责不重叠：

| 层 | 组件 | 调度对象 | 机制 |
| --- | --- | --- | --- |
| 语句调度 | `Session` | 一条 SQL 文本中的多条语句 | 顺序调度；DDL 隐式提交；逐条汇报耗时/算子次数 |
| 事务调度 | `TxnManager` | 事务 | 事务号分配、提交、整事务回滚、语句级回滚水位 |
| 锁调度 | `LockManager` | 表级 S/X 锁请求 | 条件变量等待 + **实时等待图环检测**；牺牲者＝请求者 |

**线程模型与加锁顺序**（务必保持）：存储层声明为「单线程设计」，
因此所有 `IStorage` 调用都在 `DbEngine` 的一把**递归互斥量**内进行，使并发会话的存储访问串行化。
加锁顺序固定为 **先数据库锁（`LockManager`）→ 后存储互斥量**，
且执行器从不「持有存储互斥量的同时申请数据库锁」，因此两条路径不会形成交叉等待。

### 4.3 死锁检测为什么用「实时推导」而不是缓存等待图

等待边不是维护在表里，而是在持锁状态下由「当前锁表 + 未满足请求」现场推导：

```
BlockersLocked(txn) = { 与 txn 的待满足请求冲突的持有者 }
死锁 ⟺ 从请求者出发，沿 BlockersLocked 只经过「也在等待」的事务，能回到起点
```

好处是不会因为边过期（持有者已释放但等待者还没来得及更新缓存）产生**假死锁**；
代价是每次等待前多一次小的 DFS（表数量有限，可忽略）。

---

## 5. 对原有模块的改动清单（含理由）

整合过程中**没有改动任何既有函数签名与语义**，改动全部是增量或缺陷修复：

| 文件 | 改动 | 理由 |
| --- | --- | --- |
| `cella_sql/CMakeLists.txt` | 抽出静态库 `cella_sql_core`（原 `cella_sql` exe 保留并链接它） | 让编译器可被 c++ 代码复用；exe 行为与产物不变 |
| `cella_sql/include/cella/cella_planner.h` | `CELLA_PlanNode` 追加执行期字段 + 注释说明 | 计划树升级为可执行 IR |
| `cella_sql/src/cella_planner.cpp` | 生成计划时填充上述字段 | 同上 |
| `cella_sql/src/cella_optimizer.cpp` | `cloneShell` 复制上述字段 | 保证优化后计划仍可执行 |
| `cella_sql/tests/run_tests.ps1` | 补 UTF-8 BOM | 原文件无 BOM，Windows PowerShell 5.1 按 ANSI 解码中文注释导致脚本**解析失败**（既有缺陷） |
| `cella_sql/tests/sql/ok_query_1.sql` | 修正 `nam`→`name`、删除末尾非法 `select` 语句 | 原用例自身就通不过（建表列名与查询列名不一致 + 非法语法）；golden 里本来就是 `name` |
| `cella_sql/tests/expected/ok_query_1_plan.txt` | 按修正后输入重生成（补上 Insert 段） | 与修正后的用例保持一致 |
| `cella_storage/CMakeLists.txt` | 示例路径宏由 `CMAKE_BINARY_DIR` 改为 `CMAKE_CURRENT_BINARY_DIR` | 原写法只在「本工程为顶层工程」时正确；被聚合工程 `add_subdirectory` 嵌套后指向顶层 build 目录，导致 `test_examples` 的 2 个用例失败（既有缺陷，独立构建时两者等价，改动无风险） |
| `cella_storage/**`（源码） | **零改动** | 存储模块契约稳定，无需为整合让路 |

---

## 6. 已知取舍（有意简化，逐条说明影响）

| # | 取舍 | 影响 / 边界 |
| --- | --- | --- |
| 1 | 日期族按文本存储 | 无法做日期加减；比较/排序正确（ISO 文本字典序＝时间序） |
| 2 | `UPDATE` = 删旧 + 插新 | 行物理位置变化；对有外部 Rid 引用的场景不友好（本系统 Rid 不外泄） |
| 3 | 回滚的 `DELETE`/`UPDATE` 重插旧行 | **逻辑内容精确恢复**，物理 Rid 不保证复原（存储层无「按原槽复活」接口） |
| 4 | 锁粒度 = 表 | 并发度低于行级锁；换来实现清晰与可验证的死锁检测 |
| 5 | 隔离级别 = 表级可串行化（严格 2PL） | 无 MVCC，读会阻塞写；不会出现脏读/不可重复读/幻读 |
| 6 | `GROUP BY` 无聚合函数 | 本方言未定义 `COUNT/SUM`，故实现为「按分组键去重，每组保留首行」 |
| 7 | `UNION` 两臂列数不一致 | 编译器不校验（原设计），执行层按左臂对齐、不足补 `NULL` 并告警 |
| 8 | 持久化 = 缓冲池刷盘 + 审计日志 | 事务提交默认不强制 `fsync`；`journal.log` 是审计轨迹，**不是**完整 ARIES/WAL 恢复。DDL 在自动提交后**立即存盘**；其余数据默认只在 `Close()`（干净退出）落盘，也可用 `\checkpoint` / `--checkpoint-on-commit` 手动保证 |
| 8b | 目录与数据文件不一致 → 自愈 | `catalog.meta` 在 DDL 时立即写盘，而存储层表目录要到 `Close()` 才随页刷出，进程被强杀后两者会不一致。启动时不再报 DB-509 拒绝打开，而是按目录结构重建缺失的表并告警（数据不可恢复、结构保住） |
| 9 | DDL 隐式提交前置事务 | 建表/删表立即改写目录文件，元数据不可回滚；避免「数据回滚、目录已变」的不一致 |
| 10 | `ORDER BY` 未投影列 | 计划形状固定为 `Project → Distinct → Sort`，执行期用「隐藏排序列」通道实现合法语义（详见 ARCHITECTURE.md §5.3） |
| 11 | `CatalogTable::first_page_id` | `IStorage` 未导出「首数据页」查询，建表后用 `open_table` 句柄补齐，仅作诊断展示 |
| 12 | 事务表只增不删 | `TxnManager` 保留已结束事务用于诊断；长期运行的进程需要定期重置（未来可加回收） |

---

## 7. 一页速查：把 SQL 语句映射到各层

| SQL（cella 方言） | 编译器产出 | 执行器动作 | 存储调用 |
| --- | --- | --- | --- |
| `CREATE TABLE student(id INT NOT NULL, name VARCHAR(16))` | `CreateTable` | 目录登记 + 建表 | `create_table` |
| `INSERT INTO student VALUES (1,'Alice')` | `Insert` | 逐行求值+约束 | `insert_record` |
| `get id, name in student limit id > 1` | `Project(Filter(SeqScan))` | 扫描→过滤→投影 | `open_table` + 迭代 |
| `get name in student ordered name asc` | `Sort(Project(SeqScan))` | 扫描→投影→排序 | `open_table` + 迭代 |
| `get s.name, c.title in s middle join course c on s.cid=c.id` | `Project(Join(SeqScan,SeqScan))` | 嵌套循环连接 | 两张表各一次 `open_table` |
| `UPDATE student SET name = 'x' limit id = 1` | `Update` | 扫描命中→删旧插新 | `delete_record` + `insert_record` |
| `DELETE in student limit id = 1` | `Delete` | 扫描命中→打墓碑 | `delete_record` |
| `DROP TABLE student` | `DropTable` | 释放页链 + 删目录 | `drop_table` |
| `BEGIN` / `COMMIT` / `ROLLBACK` | （编译器不支持，会话拦截） | 事务状态迁移 | — |
