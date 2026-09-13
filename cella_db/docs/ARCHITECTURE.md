# 架构设计说明（cella_db）

> 本文说明**新增的数据库层**自身的结构：组件划分、执行模型、事务与并发控制、
> 错误与日志机制、可扩展点。模块之间的接口契约见 [INTEGRATION.md](INTEGRATION.md)。

---

## 1. 层次与文件地图

```
数据库层（自顶向下）

  CLI (src/main.cpp)            交互 REPL / 脚本执行 / 元命令 / 表格化渲染
    │
  Session (engine/db_engine.*)  会话：事务上下文、逐语句调度、错误与回滚策略
    │
  DbEngine (engine/db_engine.*) 门面：生命周期、组件装配、诊断接口
    │
  Executor (exec/executor.*)    计划驱动执行：算子调度 → 存储调用
    │  ├─ ExprEval (exec/expr_eval.*)      表达式求值（三值逻辑、数值提升）
    │  ├─ RowSet   (exec/row_set.*)        内存中间结果（字段名 + 行）
    │  └─ QueryResult (exec/query_result.*) 对外结果集 + 表格渲染
    │
  CatalogManager (catalog/*)    元数据：表/列/长度/NOT NULL/首数据页/建表时间
  TxnManager + Transaction (txn/transaction.*)  事务号、undo 日志、审计日志
  LockManager (txn/lock_manager.*)              表级 S/X 锁、等待图死锁检测
    │
  IStorage (来自 cella_storage) 页式存储 + 缓冲池（内存管理）
```

| 目录 | 内容 | 是否对外导出 |
| --- | --- | --- |
| `include/cella/db/common/` | 状态码、日志门面、时间工具、类型桥 | ✅ |
| `include/cella/db/catalog/` | 目录元数据 | ✅ |
| `include/cella/db/exec/` | 执行器、求值器、结果集 | ✅ |
| `include/cella/db/txn/` | 事务与锁 | ✅ |
| `include/cella/db/engine/` | 门面、会话、SQL 文本工具 | ✅ |
| `include/cella/db/auth/` | 身份库、口令哈希、权限模型、访问控制语句解析 | ✅ |
| `src/cella/db/**` | 与头文件一一对应的实现 | ❌ |
| `tests/` | mini_test + 8 个测试文件（96 用例） | ❌ |
| `examples/` | API 速览 + 并发现场演示 | ❌ |
| `sql/` | 3 个可执行演示脚本 | ❌ |

---

## 2. 执行模型：为什么「计划驱动」而不是「AST 解释」

执行器 `Executor::Run` 的入口是一个 `CELLA_PlanNode`：

```cpp
DbStatus Executor::Run(const CELLA_PlanNode& node, const ExecContext& ctx, RowSet* out) {
  if (op == "SeqScan")   return OpSeqScan(node, ctx, out);
  if (op == "Filter")    return OpFilter(node, ctx, out);
  if (op == "Project")   return OpProject(node, ctx, out);
  if (op == "Distinct")  return OpDistinct(node, ctx, out);
  if (op == "Aggregate") return OpAggregate(node, ctx, out);
  if (op == "Sort")      return OpSort(node, ctx, out);
  if (op == "Limit")     return OpLimit(node, ctx, out);
  if (op == "Page")      return OpPage(node, ctx, out);
  if (op == "Join")      return OpJoin(node, ctx, out);
  if (op == "Union")     return OpUnion(node, ctx, out);
  return DbStatus::Error(DbCode::kNotImplemented, "执行器不支持算子: " + op);
}
```

三条理由：

1. **优化器的成果能被真正用上**。`Session` 执行的是 `cella_optimizePlans` 之后的计划，
   所以 `limit age > 10 + 8` 在执行期就是 `age > 18`，恒真 `Filter` 已被消除。
   `StatementOutcome` 同时保留 `original_plan_text` 与 `plan_text`，可对比验证。
2. **算子边界即调度边界**。新增算子（如 HashJoin、IndexScan）只需加一个 `OpXxx`，
   不必改动 SQL 前端。
3. **可观测**。`Executor::operator_calls()` 统计算子入口次数，
   测试里用它断言「计划确实被遍历了」（例如 `get name in student limit age > 19` = 4 次：
   Execute + Project + Filter + SeqScan）。

**中间结果表示**：所有算子都产出/消费同一个结构

```cpp
struct FieldRef { std::string qualifier; std::string name; };   // 限定名 + 列名
struct RowSet {
  std::vector<FieldRef> fields;                        // 列定义（顺序即列序）
  std::vector<std::vector<storage::Value>> rows;        // 行值
  int Resolve(const std::string& qualifier, const std::string& column) const;
};
```

列引用解析规则：带限定符时先精确匹配 `qualifier.name`，失败则退化为只比 `name`
（因为 `Project` 之后限定符已被丢弃，而 `Sort`/`Aggregate` 位于 `Project` 之上）。
全部比较大小写不敏感，与编译器的标识符规则一致。

---

## 3. 表达式求值：类型提升 + 三值逻辑

`ExprEval::Eval` 把编译器的 `CELLA_Expr` 求成存储层的 `Value`：

| 运算 | 规则 |
| --- | --- |
| 算术 `+ - * /` | 两侧必须数值族；两侧都是整数且结果可表示为 `int32` → 保持整数；否则提升为 `double`；除数为 0 报 **DB-511** |
| 比较 `= != < <= > >=` | 同族可比（数值↔数值、文本↔文本、BOOL↔BOOL）；任一侧 `NULL` → 结果 `NULL`（UNKNOWN）；跨族报 **DB-505** |
| `AND` / `OR` | Kleene 三值逻辑：`FALSE AND UNKNOWN = FALSE`、`TRUE OR UNKNOWN = TRUE`、其余含 UNKNOWN 即 UNKNOWN |
| `NOT` | 需 BOOL；输入 UNKNOWN → 输出 UNKNOWN |
| 一元 `-` | 需数值；输入 NULL → 输出 NULL |

**谓词语义**：`WHERE/limit`、`having`、`JOIN ON` 只接受确定为 `TRUE` 的行，
`UNKNOWN` 与 `FALSE` 一样被过滤掉（`ExprEval::EvalPredicate`）。

**NULL 的两种语义区分得很清楚**：

* 表达式中：`NULL = NULL` → UNKNOWN（`CompareValues` 的 `known=false`）；
* 分组/去重中：`NULL` 与 `NULL` 视为同一组（`ValueEquals`）。

---

## 4. 事务与并发控制

### 4.1 组件

```cpp
class Transaction {   // 单线程使用
  txn_id_t id_; TxnState state_;              // ACTIVE / COMMITTED / ABORTED
  std::vector<UndoRecord> undo_;              // 逻辑 undo 日志
  std::set<std::string> touched_;             // 触碰过的表（审计用）
  size_t statements_;                         // 语句计数
};

class TxnManager {    // 跨线程共享，内部互斥量保护
  txn_id_t Begin();
  DbStatus Commit(txn_id_t);
  DbStatus Abort(txn_id_t, const std::string& reason);
  size_t   UndoMark(txn_id_t);                              // 取 undo 水位
  DbStatus RollbackToMark(txn_id_t, size_t mark, const std::string& reason);
};

class LockManager {   // 表级 S/X，条件变量 + 实时等待图
  DbStatus Acquire(txn_id_t, const std::string& resource, LockMode);
  void      ReleaseAll(txn_id_t);
};
```

### 4.2 原子性：逻辑补偿式 undo

存储层只提供「追加插入」与「打墓碑删除」，删除后无法按原 `Rid` 复活，
因此 undo 采用**逻辑补偿**而非物理页回滚：

| 变更 | undo 记录 | 回滚动作 |
| --- | --- | --- |
| `INSERT` | `{kInsert, table, rid}` | `delete_record(rid)` — 把自己插的那行标记删除 |
| `DELETE` | `{kDelete, table, rid, before}` | `insert_record(before)` — 按内容重插 |
| `UPDATE` | `{kUpdate, table, new_rid, before}` | `delete_record(new_rid)` + `insert_record(before)` |

对可观察语义（行内容）而言回滚是精确的；物理 `Rid` 不保证复原，
这一点在 INTEGRATION.md §6 明确列为取舍 #2/#3。

### 4.3 语句级原子性

`Session::ExecuteOne` 在语句执行前取一次 `UndoMark`；若语句失败且处于显式事务中，
调用 `RollbackToMark` **只撤本语句的 undo**（不结束事务、不释放锁）。
这保证了 `INSERT ... VALUES (5,500),(6,1.5)` 这种「第一行成功、第二行失败」的语句
不会留下半截数据：

```
BEGIN;
INSERT INTO account VALUES (6,600),(7,1.5);   → DB-505，undo 撤回 id=6
get id in account;                            → 看不到 id=6（事务仍可用）
INSERT INTO account VALUES (6,600);           → 成功
COMMIT;
```

### 4.4 隔离：严格两阶段封锁（Strict 2PL）

* 加锁时点：算子访问表时按需加锁（读 S / 写 X）。
* 释放时点：**只在提交或回滚时统一释放**（`LockManager::ReleaseAll`）——
  锁持有到事务结束，因此不会出现「读了但锁已放」的级联回滚问题。
* 锁升级：持 S 再请 X 时按「等待其它共享持有者」处理；两个事务互相升级会成环，
  由死锁检测选出牺牲者。
* 并发效果：等价于**表级可串行化**；无脏读、不可重复读、幻读。

### 4.5 死锁检测

```
BlockerLocked(txn) : txn 的待满足请求与当前锁表冲突的持有者集合
死锁 ⟺ 从请求者出发，沿 Blocker 边（只经过「也在等待」的事务）能走回自己
```

检测到环时：**请求者**被选为牺牲者，`Acquire` 返回 `DB-604`（`kDeadlock`），
`Session` 收到该错误码后执行**整事务回滚**并释放锁，被阻塞的一方随即获得锁。

选「请求者」当牺牲者而不是「最小事务号」的原因：不需要维护事务年龄信息，
且能保证等待方一定推进（避免饥饿）。`DumpWaitForGraph()` 可打印实时等待边，
死锁演示里能看到 `11 -> 10 -> 11` 这样的环。

### 4.6 超时兜底

即使死锁检测漏判（例如实现缺陷），每个加锁请求也带 `lock_timeout`（默认 5000ms），
超时返回 `DB-605`（`kLockConflict`）。这是「安全网」而不是主机制。

### 4.7 线程模型

| 事实 | 后果 |
| --- | --- |
| `IStorage` 声明为非线程安全 | 所有存储调用在 `DbEngine` 的**递归互斥量**内串行执行 |
| 加锁顺序固定：数据库锁 → 存储互斥量 | 不会与「持存储锁等数据库锁」形成交叉等待 |
| `LockManager`/`TxnManager` 自带互斥量 | 多会话从不同线程并发使用是安全的 |
| 每个 `Transaction` 单线程使用 | 会话之间不共享事务对象 |

### 4.8 持久化与「存盘点」（Checkpoint）

存储层没有 WAL，脏页只在 `Close()` 时 `FlushAllPages` 落盘。目录（系统表 `cella_catalog`）
与用户数据现在都住在同一个 `cella.db` 里、走同一条页式持久化路径，所以「目录有表、
数据文件没表」这类**结构性不一致已不存在**——但 DDL 若不在进程被强杀前落盘，整张表
（目录行 + 物理表目录）会一起丢。

为此数据库层保留了两点：

| 机制 | 行为 |
| --- | --- |
| `DbEngine::Checkpoint()` | 用公开接口 `Close()+Open()` 组合实现一次「存盘」（`IStorage` 没有 flush-all），调用期间独占存储互斥量 |
| DDL 后自动存盘 | `Session` 在 `CREATE/DROP TABLE` 自动提交成功后立刻 `Checkpoint()`，保证 DDL 的效果立即持久化 |

数据（DML）默认仍是「干净退出才落盘」；需要更严格时可开启 `--checkpoint-on-commit`
（每次提交都存盘），或在 REPL 里用 `\checkpoint` 手动触发。存盘点会重建缓冲池
（统计计数器归零），因此 `StatsText()` 会把历史值累计下来，保证观测连续。

---

## 5. 三个值得一提的实现细节

### 5.1 「隐藏排序列」通道（ORDER BY 引用未投影列）

计划形状由编译器固定为 `... → Project → Distinct → Sort → Limit → Page → Union`，
而 SQL 语义要求 `ORDER BY` 作用在投影**之前**，所以

```sql
get name in student ordered age desc;    -- age 不在选择列表里
```

在计划上无法直接执行。为了不改动计划文本（golden 是契约），执行期这样做：

```
OpSort   把需要的排序键放进 sort_hint_，然后跑子树
OpProject 在正常输出列之后追加同序的「隐藏键列」（键表达式的值）
OpDistinct 只按可见前缀去重（隐藏列不参与 DISTINCT 语义）
OpSort   排序（可见列优先解析，解析不到才用隐藏列），最后把隐藏列裁掉
```

对外结果与 SQL 语义一致，计划文本与优化器完全不受影响。

### 5.2 两阶段修改（DELETE / UPDATE）

存储层的迭代器在页内游走，边扫描边改页会破坏迭代。
因此 `ScanMatching` 先收集 `(Rid, Record)` 列表，再逐条修改。
这也是 undo 需要保存 `before` 记录的原因。

### 5.3 目录同步与语义阶段隔离

* `Session::CompileOnly`（`--show-plan`、`\plan`）在目录**副本**上做语义分析，
  因此「只看计划」不会改变会话状态。
* `Session::ExecuteOne` 在每条语句后 `EnsureCatalogInSync()`
  重建编译器目录，既让 `CREATE TABLE` 立即可见，也修复语义阶段可能的目录突变。

---

## 6. 错误处理与日志

### 6.1 错误码空间

| 段 | 含义 | 例 |
| --- | --- | --- |
| `LEX/SYN/SEM/PLN-xxx` | 编译器诊断（原样透传） | `SEM-307 不能向 NOT NULL 列插入 NULL` |
| `DB-5xx` | 目录/执行/多库 | `DB-502 表不存在`、`DB-505 类型不匹配`、`DB-508 文本超长`、`DB-511 除零`、`DB-512 系统表只读`、`DB-513 事务中禁切库`、`DB-514 库不存在/已存在/名非法`、`DB-515 目标库不可删除`、`DB-516 主键冲突` |
| `DB-6xx` | 事务/并发 | `DB-601 无活动事务`、`DB-602 重复 BEGIN`、`DB-604 死锁`、`DB-605 锁超时` |
| `DB-7xx` | 目录/会话/CLI | `DB-701 目录操作失败`、`DB-702 会话状态错误`、`DB-703 未实现`、`DB-704 内部错误` |
| `DB-8xx` | 访问控制 | `DB-801 认证失败`、`DB-802 权限不足`、`DB-803 用户问题`、`DB-804 最后一个管理员`、`DB-805 非管理员授予`、`DB-806 未登录` |

`DbStatus` 带 `[[nodiscard]]`，忽略返回值即编译告警（与存储层 `Status` 一致）。
存储层错误经 `FromStorage()` 映射，**保留原始信息**（消息里附 `[存储: ...]`）。

### 6.2 日志体系

| 文件 | 产出方 | 内容 |
| --- | --- | --- |
| `<data_dir>/cella-storage.log` | 存储层 | 页级 I/O、缓冲池命中/淘汰、pin/unpin |
| `<data_dir>/cella-db.log` | 数据库层 | `catalog`/`exec`/`txn`/`lock`/`session`/`engine` 六类 |
| `<data_dir>/journal.log` | `TxnManager` | 每个 `COMMIT`/`ABORT` 一行（时间戳 + 事务号 + 语句数 + undo 数 + 涉及表） |

数据库层日志复用存储层的 `ILogger` 抽象（`ConsoleLogger`/`FileLogger`），
另有 `TeeLogger` 支持「文件 + 控制台」双写。级别由 `--log` 控制（`debug|info|warn|error|off`）。

### 6.3 输出渲染

`QueryResult::ToText()` 输出对齐的表格；列宽按 **UTF-8 显示宽度**计算
（ASCII 记 1、CJK 记 2），因此中英混排的列也能对齐。

---

## 7. 可扩展点

| 想扩展的功能 | 建议落点 |
| --- | --- |
| 新算子（哈希连接、索引扫描、Top-N 堆） | `Executor::Run` 加一个 `OpXxx`；若需要前端配合，在 `cella_planner.cpp` 生成对应 `op` |
| 新 SQL 语法 | `cella_sql`（lexer/parser/semantic/planner），本层通常只需支持新算子 |
| 新数据类型 | `value_bridge.cpp` 的类型映射 + `CoerceValue`；若存储层不支持物理类型则按文本落地 |
| 行级锁 / MVCC | `LockManager` 增加资源键粒度（`Rid` 作为键）；隔离级别提升到「读不阻塞写」需要版本链 |
| 缓冲池策略 | 加一个 `IReplacer` 实现 + `ReplacerFactory` 注册；数据库层只用 `EngineConfig.replacer` |
| 索引 | 存储层已预留 `PageType::kIndexPage`；在 `Executor` 增加 `IndexScan`，目录里登记索引元数据 |
| 恢复（ARIES/WAL） | `journal.log` 升级为带 `LSN` 的 redo/undo 日志，`DbEngine::Open` 增加重放阶段 |
| 更高并发度 | 把存储互斥量换成「页级闩锁 + 表级意向锁」；执行器已是按算子粒度访问存储，改动面可控 |
| 新日志后端 | 实现 `storage::ILogger` 即可（如网络日志、结构化 JSON） |

---

## 8. 构建与产物

| 产物 | 说明 |
| --- | --- |
| `build/cella_db/cella_db.exe` | 完整系统 CLI（REPL / 脚本 / 元命令） |
| `build/cella_db/cella_db_tests.exe` | 96 用例 / 848 断言 |
| `build/cella_db/api_quickstart.exe` | 30 秒 API 速览 |
| `build/cella_db/concurrency_demo.exe` | 并发与死锁现场演示 |
| `build/cella_sql/cella_sql.exe` | 原编译器 CLI（保留，行为不变） |
| `build/cella_storage/storage_demo.exe` 等 | 原存储层 demo / 测试（保留） |

编译要求：C++17、MSVC 或 g++；`cella_storage` 与 `cella_db` 在 MSVC 下以
`/W4 /WX`（g++ 下 `-Wall -Wextra -Werror`）构建，**零警告**。
