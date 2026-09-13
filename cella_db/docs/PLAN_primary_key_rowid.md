# 主键与 rowid 实现计划

> 目标：让「按行定位」成为语言级能力，解决「同值重复行无法精确 UPDATE/DELETE」的结构性缺口。
> 这是可编辑表格客户端（Navicat 式）M2 的前置条件。
>
> 前置事实（已到代码里核实）：
> - `TableHeap` 已有 `GetRecord(rid, out)` / `DeleteRecord(rid)` / `InsertRecord`，`IStorage` 只暴露
>   `insert_record` / `delete_record(rid)`。→ **按 Rid 读/删无需改存储层**。
> - `Rid = {page_id, slot_id}` 只在存储层内部存在，SQL 层不暴露。
> - `UPDATE` 实现为「删旧 + 插新」→ 物理 Rid 会变；墓碑槽位后来可能被插入复用。
> - 目录列的文本编码是 `名 类型 长度 非空` 四段（`DecodeColumns` 严格要求 4 段）。
> - 计划打印是 golden 契约：**既有 30 个用例的输出必须逐字节不变**。

---

## 0. 验收标准

| 缺口 | 验收 |
| --- | --- |
| 无主键 | `CREATE TABLE t(id INT PRIMARY KEY, ...)` 可用；重复值/NULL 写入被拒（DB-516 / DB-506）；重启后主键定义保留；`\d` 可见 |
| 无 rowid | `get rowid, * in t` 返回行标识；`delete in t limit rowid = X` 精确删一行（**同值重复行也能精确删**）；`update ... limit rowid = X` 同理 |
| 兼容 | 既有 30 个编译器 golden 输出逐字节不变；既有 `.db`（含 `mydb/`）仍可打开；三层回归全绿 |

---

## 1. 顺序：先主键，后 rowid

先主键的理由：

1. 主键是**纯逻辑约束**，改动收敛在「语法 + 目录 + 执行器校验」，不碰存储层的行标识语义；
2. 主键一旦存在，客户端就有了**安全**的行定位方式（唯一 + 非空），rowid 退回「给没有主键的老表兜底」；
3. rowid 有语义陷阱（UPDATE 后变化、删除后复用），在主键已经能覆盖大部分场景的前提下再引入，风险更可控；
4. 两者独立，可分别提交、分别回归，任一阶段出问题都能单独回滚。

---

## 2. 阶段一：PRIMARY KEY

### 2.1 设计决策

| 决策点 | 结论 | 理由 |
| --- | --- | --- |
| 语法位置 | **列级**：`id INT PRIMARY KEY` | 单列主键覆盖绝大多数场景；表级 `PRIMARY KEY (a,b)` 列为可选扩展（见 §4） |
| 隐含约束 | 主键列**自动 NOT NULL** | 标准语义；复用既有 DB-506 报错 |
| 一张表几个主键 | 至多一个，重复声明报 SEM 错 | 与标准一致 |
| 唯一性怎么查 | 插入/更新时**扫描已有行比对**（无索引，O(n)） | 不做索引（另立项）；教学规模可接受，文档写明 |
| 批量插入 | 语句内**累积键集合**一次比对（不逐行重扫） | 否则 N 行插入退化 O(N²) |
| 存储层 | **零改动**：主键不改变行的物理布局 | 主键只是约束 + 元数据 |
| 目录编码 | 列编码加**第 5 段** `主键标志(0/1)`；解码**兼容 4 段旧格式** | 旧 `.db`/`catalog.meta` 迁移路径不受影响 |
| 新增错误码 | `DB-516 主键冲突` | 与既有码段一致 |

### 2.2 改动文件清单

| 文件 | 改动 | 量 |
| --- | --- | --- |
| `cella_sql/include/cella/cella_token.h` | 关键字加 `PRIMARY`、`KEY` | 小 |
| `cella_sql/include/cella/cella_ast.h` | `CELLA_ColumnDef` 加 `bool primaryKey` | 小 |
| `cella_sql/src/cella_parser.cpp` | 列定义尾部识别 `PRIMARY KEY` | 小 |
| `cella_sql/src/cella_semantic.cpp` | 至多一个主键；主键列置 `notNull=true`；重复声明报错 | 小 |
| `cella_sql/src/cella_printer.cpp` | `columnTypeText` 对主键追加 ` PRIMARY KEY`（仅新语句出现，既有 golden 不变） | 小 |
| `cella_db/include/cella/db/catalog/catalog_manager.h` | `CatalogColumn` 加 `bool primary_key`；`CatalogTable::PrimaryKeyColumnIndex()` | 小 |
| `cella_db/src/cella/db/catalog/catalog_manager.cpp` | 编码第 5 段 + 兼容解码；`DescribeTable` 显示 PRIMARY KEY | 小 |
| `cella_db/src/cella/db/exec/executor.cpp` | ① 建表：主键标志写入目录行；② INSERT：键集合唯一性校验（DB-516）；③ UPDATE：涉主键列时「排除自身」查重 | 中 |
| `cella_db/include/cella/db/common/db_status.h/.cpp` | `DB-516` | 小 |
| `cella_db/src/main.cpp` | `\d` 已走 DescribeTable，自动可见 | 无 |

### 2.3 实施步骤

1. [x] 关键字 + AST + 解析器（`PRIMARY KEY` 列级）
2. [x] 语义：主键隐式 NOT NULL、至多一个、与显式 NOT NULL 不冲突
3. [x] 目录：`CatalogColumn::primary_key`、编码第 5 段、**兼容 4 段解码**、DescribeTable
4. [x] 执行器建表：主键标志落目录
5. [x] 执行器 INSERT：语句级唯一性校验（累积集合，冲突 → DB-516；失败走既有语句级回滚）
6. [x] 执行器 UPDATE：涉主键列时按「排除自身」查重
7. [x] 编译器 golden 新用例 `ok_primary_key.sql` + 期望计划
8. [x] 整合层测试（见 2.4）
9. [x] 文档：`grammar.md`、`INTEGRATION.md`、`ARCHITECTURE.md`、`TEST_REPORT.md`、README 方言表
10. [x] 全量 `run_all.ps1` + 提交

### 2.3b 阶段一完成记录（2026-09-12）

* 全部步骤完成；回归：编译器 **32/32**（新增 ok_primary_key + err_pk_multi），
  整合层 **86/86（740 断言）**，既有 golden 未动一字节。
* 实施中发现的两处要点：
  1. **计划文本有两条渲染路径**：`-p` golden 走 `cella_planner.cpp` 的 `extra` 文本，
     而 `--all` 语句转储走 `cella_printer.cpp::columnTypeText` —— 两处都要加 `PRIMARY KEY`，
     只改打印器会导致 golden 里看不到主键（排查过一次）。
  2. **向后兼容用真实路径验证**：除手工构造 4 段编码目录行的单测外，
     还用新版本成功打开了改造前建的真实库 `mydb/`（4 段编码）——真实兼容性已确认。
* 目录编码从 4 段扩到 5 段（末位主键标志），解码同时接受 4/5 段。
* 存储层零改动（与计划一致）。

### 2.4 测试清单

| 用例 | 断言 |
| --- | --- |
| `主键_建表与显示` | 建表成功；`\d`/DescribeTable 含 PRIMARY KEY；重启后保留 |
| `主键_重复插入被拒` | 同值二次插入 → DB-516；原数据不变 |
| `主键_批量插入部分冲突整体回滚` | 3 行插入、第 3 行冲突 → 一行都不留（语句级原子性） |
| `主键_NULL被拒` | `INSERT ... VALUES (NULL)` → DB-506（主键隐含非空） |
| `主键_更新撞车被拒` | `UPDATE` 把 PK 改成已存在的值 → DB-516；改成自己原值 → 允许（排除自身） |
| `主键_删除后可重用` | 删除 PK=1 后，可再插入 PK=1 |
| `主键_旧库兼容` | 改造前建的库（4 段编码）仍能打开、读写正常 |
| `主键_编译器golden` | `ok_primary_key` 计划文本与 golden 一致 |

### 2.5 风险与对策

| 风险 | 对策 |
| --- | --- |
| 目录编码格式变更导致旧库读不出 | 解码**同时接受 4 段与 5 段**；补一个「旧格式库」回归用例把这条钉死 |
| 既有 golden 被打破 | 打印只在主键存在时追加文本；既有用例无主键 → 输出不变；回归必须 30/30 |
| 批量插入 O(N²) | 语句内累积键集合；复杂度写进文档 |
| 主键无索引 → 无性能收益 | 文档明确「主键是约束不是加速器」，索引另立项 |

---

## 3. 阶段二：rowid（物理行标识伪列）

### 3.1 设计决策

| 决策点 | 结论 | 理由 |
| --- | --- | --- |
| 定义 | `rowid` = 物理行标识（页号 + 槽号），值 = `(page_id << 16) \| slot_id` 的**不透明整数** | 不透明 → 用户不会做算术；`<<16` 允许每页 65535 槽，足够 4KB 页 |
| 值类型 | `kInt64` | 保证大库不溢出；需核实/补齐 `int32 字面量 vs int64 值` 的比较路径 |
| 可用位置 | 投影 / `limit` / `ordered`；**不可** INSERT 指定、**不可** UPDATE 修改 | 物理标识由引擎产生，用户不可写 |
| 保留名 | `rowid` 进关键字表 → 不能作列名/表名（报 SEM 错） | 避免「用户列 vs 伪列」歧义 |
| UPDATE 后稳定性 | **不保证**（UPDATE = 删旧+插新） | 文档明示；不做 in-place update（见 §4 可选扩展） |
| 删除后复用 | **可能复用**（槽位回收） | 缓解：客户端在同一持锁事务内 fetch→改；文档写明 |
| 快速路径 | `limit rowid = X` 走 `TableHeap::GetRecord(rid)` 直接定位 | 顺带得到一个 O(1) 的按行访问能力 |
| 存储层 | **零改动**（`GetRecord`/`DeleteRecord` 已有） | 唯一需要补的是 SQL 侧的类型比较路径 |

### 3.2 改动文件清单

| 文件 | 改动 | 量 |
| --- | --- | --- |
| `cella_sql/include/cella/cella_token.h` | 关键字加 `ROWID` | 小 |
| `cella_sql/src/cella_semantic.cpp` | 每张表作用域合成 `rowid` 伪列（INT）；INSERT 列清单/SET 目标出现 rowid → SEM 错 | 中 |
| `cella_db/include/cella/db/exec/row_set.h` | RowSet 增加可选 `rid` 列（每行一个 `storage::Rid`） | 小 |
| `cella_db/src/cella/db/exec/executor.cpp` | SeqScan 填充 rid；投影/谓词解析 rowid；`limit rowid = X` 走 GetRecord 快速路径 | 中 |
| `cella_db/src/cella/db/exec/expr_eval.cpp` | 列引用 `rowid` → 从行 rid 取值；补齐 int64 参与的比较 | 中 |
| `cella_db/include/cella/db/exec/executor.h` | 执行期标志（如 `needs_rowid`）——**若用计划节点附加字段，必须同步改 `cella_optimizer.cpp::cloneShell`** | 小 |

### 3.3 实施步骤

1. [x] 关键字 `ROWID` + 语义合成伪列（投影/WHERE/ORDER BY 可解析）
2. [x] 非法用法拦截：INSERT 指定 rowid、UPDATE SET rowid、CREATE TABLE 用 rowid 作列名
3. [x] RowSet 承载 rid；SeqScan 填充
4. [x] expr_eval：rowid 取值 + 数值比较路径（int32/int64 混合）
5. [x] 快速路径：`limit rowid = X` → `GetRecord` 定向读写（DELETE/UPDATE 共用）
6. [x] 编译器 golden 新用例 `ok_rowid.sql`
7. [x] 整合层测试（见 3.4）
8. [x] 文档 + 全量回归 + 提交

### 3.3b 阶段二完成记录（2026-09-12）

* 全部步骤完成；回归：编译器 **33/33**（新增 ok_rowid），整合层 **87/87（765 断言）**，
  既有 golden 未动一字节。
* 实施中的两个关键决策（与计划的偏差/澄清）：
  1. **rowid 不进关键字表**：仍按普通标识符解析，只在语义的「列存在性」判定上把 rowid 视为
     每张表都有的伪列。这样 INSERT 列清单 / SET 目标天然查不到它（走 `CELLA_Catalog::findColumn`），
     连「禁止写入」都不需要额外代码；只有「声明为列名」需要显式拦（新码 SEM-314）。
  2. **按需附加，而非总是附加**：`get *` 没有 Project 节点（星号展开在编译期完成），
     若总是附加 rowid 就会泄漏成结果里的一列。改为按语句判定（`StmtRefersRowid` →
     `ExecContext::with_rowid`），只有真正引用 rowid 的语句才附加，`get *` 输出与改造前逐字节一致。
     标志放在 ExecContext（而非 Executor 成员）是因为引擎会被多会话并发使用。
* 踩坑记录：GET 的条件存在 `st->limit`（方言 limit = WHERE），DELETE/UPDATE 的存在 `st->where` ——
  语句级 rowid 判定必须两处都看，否则 `get ... limit rowid = X` 会报「列不存在」。
* 快速路径：`rowid = <整数>` 走 `TableHeap::GetRecord` 直达（O(1)），越界值返回空命中而非报错，
  存储错误时回退全表扫描；结果语义与扫描路径完全一致。
* 存储层零改动（`GetRecord` / `DeleteRecord` 早已存在）。

### 3.4 测试清单

| 用例 | 断言 |
| --- | --- |
| `rowid_投影返回整数` | `get rowid, id in t` 每行一个正整数，两列对齐 |
| `rowid_精确删重复行` | 造两行全列相同的行 → `delete ... limit rowid = X` 只删掉一行（**全列匹配做不到的唯一价值**） |
| `rowid_精确改重复行` | 同上场景用 rowid 更新其中一行，另一行不变 |
| `rowid_无修改时稳定` | 同一会话两次 `get rowid` 值一致 |
| `rowid_更新后变化` | `UPDATE` 后该行 rowid 改变（文档化断言，避免误依赖） |
| `rowid_不存在返回空` | `limit rowid = 999999` → 空结果、不报错 |
| `rowid_非法写入被拒` | `INSERT INTO t(rowid, id) ...` / `UPDATE t SET rowid = 1` → 编译错误 |
| `rowid_保留名` | `CREATE TABLE rowid(...)` 或列名 rowid → SEM 错 |
| `rowid_编译器golden` | `ok_rowid` 计划文本与 golden 一致 |

### 3.5 风险与对策

| 风险 | 对策 |
| --- | --- |
| 槽位复用导致陈旧 rowid 指向别的行 | 客户端用法约定：**同一持锁事务内 fetch→改**；无锁场景文档警告；未来可加槽位代数（需改存储格式，本次不做） |
| int64 比较路径缺失 | 实施第 4 步先写一个 int64 比较单测；若执行器只支持 int32，则退回「int32 rowid + 页号上限校验」并在文档标注 |
| 伪列污染计划打印（golden） | 打印只输出 `rowid` 字面名；既有用例不含 rowid → 输出不变 |
| UPDATE 后 rowid 变化被误用 | 测试显式断言该行为 + 客户端接口层提供「改完重新 fetch」的封装 |

---

## 4. 明确不做 / 留待扩展

| 项 | 说明 |
| --- | --- |
| 表级复合主键 `PRIMARY KEY (a,b)` | 与单列主键共用同一套校验（键元组比对），但要多解析一个表级约束子句；建议**本迭代先单列**，需要再做 |
| 索引 | 主键不建索引（唯一性靠扫描）；索引是独立立项（存储层有 `PageType::kIndexPage` 预留） |
| `UNIQUE` / `FOREIGN KEY` | 与主键同族的约束，可后续按同一模式扩（校验点相同） |
| `ALTER TABLE`（增删主键） | 仍未实现；改主键需重建表 |
| in-place update（保持 rowid 稳定） | 需存储层新增 `update_record_at`（槽内原地改，长度变化时回退删插）；可显著改善 rowid 稳定性，列为后续增强 |
| rowid 的跨表全局唯一 | 当前 rowid 只在表内唯一（页号是表内相对），文档写明 |

---

## 5. 两个特性的关系与客户端行定位矩阵

| 表情况 | 客户端生成的行定位条件 | 安全性 |
| --- | --- | --- |
| **有主键** | `limit <pk> = <值>` | ✅ 最安全（唯一 + 非空 + 不随 UPDATE 变） |
| 无主键 | fetch 时选 `rowid`，编辑时 `limit rowid = <值>`（同一持锁会话） | ⚠️ 可用；需持锁，且改完要重新 fetch |
| 都不适用（无锁/外部改动） | 全列匹配 + 影响行数预检（>1 行则拒绝/确认） | ⚠️ 最后手段（现方案 ①） |

**对客户端接口的约定**：`get` 结果结构里同时携带（可选）`rowid` 列与（若有）主键值；
客户端优先用主键生成 WHERE，其次 rowid，最后全列匹配。这样 M2 可编辑网格的上限就打开了。

---

## 6. 待你拍板的三点

1. **主键范围**：本迭代只做列级单列主键（建议），还是同时做表级复合 `PRIMARY KEY (a,b)`？
2. **rowid 稳定性**：接受「UPDATE 后 rowid 变化 + 删除后可能复用」（建议，零存储层改动），
   还是顺手把存储层 `update_record_at` 也做了（rowid 在长度不变时保持稳定）？
3. **顺序**：按建议「主键 → rowid」，还是反过来（若你更急着让客户端能精确改**已有的无主键表**，rowid 先做也合理）？
