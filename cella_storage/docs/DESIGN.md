# DESIGN.md — 设计文档

> 读者：想理解存储系统内部设计的组员 / 评审者。

---

## 1. 架构图

```
数据库引擎(组员B) → PlanBridge(stub) → IStorage → TableHeap/Record → BufferPool → Page → IDiskManager
                                            ↑ 所有层依赖 common(config/status/logger/types/value/schema/record/buffer_stats)
对外只暴露:  IStorage + common/ + table/        内部实现: disk/ page/ buffer/ record/
```

- 依赖严格单向：上层只能依赖下层，严禁反向。
- `IStorage` 是唯一契约面；引擎组物理上 include 不到 `disk/page/buffer/record`。

## 2. 分模块职责

| 模块 | 职责 | 扩展点 |
| --- | --- | --- |
| `disk/` | 页级 I/O、空闲页链表、MetaPage | `IDiskManager`（File/Mem） |
| `page/` | Page 对象、页头、槽式布局、MetaPage 编解码 | `PageLayout`（Slotted） |
| `buffer/` | 缓冲池、淘汰策略、RAII、统计 | `Replacer`（LRU/FIFO/CLOCK） |
| `record/` | 记录序列化 | `IRecordSerializer`（slotted） |
| `table/` | 表→页映射、TableHeap、迭代器 | — |
| `api/` | 统一接口、工厂 | `IStorage`（FileStorage） |

## 3. 页结构与二进制格式

- **页偏移** `offset = page_id * page_size`；`page_id=0` = MetaPage。
- **数据页槽式布局**：

```
┌ PageHeader 32B ┬ 槽数组(向后增长) ┬ 空闲空间 ┬ 记录数据(从页尾向前) ┐
                    每项4B{offset:u16,len:u16}        len==0 = 已删除
剩余空间 = free_end - (32 + slot_count*4)
```

- **MetaPage**：`magic "CELLADB1" | format_version | page_size | page_count | free_list_head | catalog_root_page | checksum`。
- **记录**：`col_count:u16 | null_bitmap | 列数据`；VARCHAR 前缀 `len:u16`。
- 所有字段**小端**，逐字节读写（`byte_buffer.h` 的 `PutUint*/GetUint*`），**禁止 memcpy 结构体**。

## 4. 缓冲池与淘汰流程

帧 = `{page_id, pin_count, is_dirty, Page}`。`page_table_` 是 `page_id→frame` 哈希，`free_frames_` 是空闲帧栈。

**淘汰流程（`find_free_frame`）**：
1. 有空闲帧 → 直接用。
2. 否则 `Replacer::Victim()` 选牺牲帧（策略决定）。
3. 牺牲帧若脏 → 先写盘（`dirty_flush++`）。
4. 清空旧页映射、复用该帧（`evict++`、记替换日志）。

**关键不变量**：`pin_count == 0` 的帧才在 Replacer 里（`UnpinPage` 归零时 `Insert`，`get_page` pin 时 `Pin`）；全 pin 时 `Victim` 返回 false → `get_page` 返回 nullptr（对应 `kNoFreeFrame`），**不死锁不崩溃**。

### LRU / FIFO 伪代码与复杂度

**LRU**（双向链表 + 哈希，头=最近，尾=最久）：
```
Insert(f): if f in map: erase; push_front(f); map[f]=begin
Pin(f):    if f in map: erase; return true; else false
Victim():  if empty: false; else pop_back → victim
```
复杂度：均摊 O(1)。

**FIFO**（插入顺序队列 + 可淘汰集合）：
```
Insert(f): if f evictable: return; evictable.insert(f); if f 首次: queue.push_back(f)
Pin(f):    if f evictable: evictable.erase(f); return true; else false
Victim():  从队头找第一个 evictable 帧 → 淘汰
```
复杂度：Victim O(n)。**关键**：Pin 只移出可淘汰集、不改插入顺序——因此 FIFO 按「最先加载」淘汰、LRU 按「最近访问」淘汰，同一负载下结果不同（否则二者等价）。

**CLOCK**（环形时钟 + 引用位，近似 LRU）：`Victim` 转圈，遇 `ref=1` 清 0 给第二次机会，遇 `ref=0` 淘汰。

## 5. 表→页映射

- 一张表 = 一条**数据页双向链表**（页头 `prev_page_id`/`next_page_id`）。
- 表名 → (schema, 首数据页) 的**目录**持久化在 `kCatalogPage`，由 `MetaPage.catalog_root_page` 引用。
- `TableHeap::InsertRecord`：走到最后一页，插不下则 `NewPage` 分配新页挂尾。
- `TableIterator`：跨页扫描，槽 `len==0`（墓碑）自动跳过。
- catalog 的**富语义**（索引、约束、统计）归引擎组，用 `read_page`/`write_page` 把 catalog 当特殊表存。

## 6. 与执行计划对接时序

```
逻辑执行计划
  ├ SeqScan  → IStorage::open_table + TableHeap::begin/end
  ├ Filter   → 引擎组内存内谓词过滤（对扫描结果）
  ├ Project  → 引擎组内存内取列/取前 N
  ├ Insert   → IStorage::insert_record
  └ Delete   → IStorage::delete_record（标记删除）
```

B+ 树（`PageType::kIndexPage`）、事务、并发为**预留接入点**，本次不实现。

## 7. 六大扩展点

| 扩展点 | 抽象 | 现有实现 | 扩展方式 |
| --- | --- | --- | --- |
| 替换策略 | `Replacer` | LRU/FIFO/CLOCK | 新文件 + `ReplacerFactory::Register`，**核心零改动** |
| 磁盘后端 | `IDiskManager` | File/Mem | 新文件即可 |
| 存储实现 | `IStorage` | FileStorage | 新文件即可（内存库/加密库） |
| 页布局 | `PageLayout` | Slotted | 新文件即可 |
| 记录序列化 | `IRecordSerializer` | slotted | 新文件即可 |
| 数据类型/日志 | `ValueType` 集中 switch / `ILogger` | Console/File | 改枚举一处 / 新文件 |

## 8. 与 MySQL/InnoDB 对照

| 维度 | cella 存储系统 | InnoDB |
| --- | --- | --- |
| 页大小 | 4KB（可配置 512~64KB） | 16KB（默认） |
| 页布局 | 槽式（PageHeader + slot array） | 槽式页（Page Directory + 记录堆） |
| 缓冲池 | LRU/FIFO/CLOCK（可插拔） | LRU（young/old 双区，防全表扫描污染） |
| 空闲页 | MetaPage 头指针 + 页头 next 串链 | 段/区（fsp0fsp），空闲链表 |
| 删除 | 标记删除（墓碑） | 标记删除 + purge 后台物理回收 |
| 目录 | kCatalogPage 存最小表→页映射（引擎组扩展富语义） | 数据字典（SYS_TABLES 等系统表） |
| 崩溃恢复 | 无 WAL（Close 时落盘） | redo/undo 日志 + 双写缓冲 |
| 事务/并发 | 预留，不实现 | MVCC + 行锁 |

> 本模块是**教学级物理底座**：结构对齐 InnoDB 的页/缓冲池概念，但刻意省略 WAL、事务、purge 等，把复杂度留给后续阶段。
