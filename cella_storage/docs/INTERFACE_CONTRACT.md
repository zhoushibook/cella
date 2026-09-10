# cella 存储系统 — 接口契约（对外主文档）

> 读者：**不读源码的数据库引擎组组员（组员 B）**。
> 你只需要看这份文档 + `API.md`，就能调用存储系统，无需理解内部实现。

- **接口版本**：`cella-storage/0.1`（`IStorage::InterfaceVersion()` 返回该值）
- **语言/标准**：C++17
- **命名空间**：`cella::storage`
- **构建产物**：静态库 target `cella_storage`

---

## 1. 概述

存储系统是数据库引擎的**物理底座**：页式存储 + 缓冲池 + 统一访问接口。引擎组只依赖一个头文件：

```cpp
#include "i_storage.h"      // 对外唯一契约面
```

外加 `common/`（公共类型）与 `table/`（表访问接口）。**内部目录 disk/page/buffer/record 不导出**，物理上 include 不到。

---

## 2. 30 秒上手

下面是最小可运行示例（真实源码见 `examples/quickstart.cpp`，已被测试编译 + 运行通过）：

```cpp
#include "i_storage.h"
#include "storage_factory.h"
#include "schema.h"
#include "record.h"
#include "value.h"
#include "table_heap.h"

int main() {
  using namespace cella::storage;

  StorageConfig config;                 // 默认配置（page_size=4096, pool=64, LRU）
  auto storage = CreateStorage(config);
  if (!storage->Open(config).ok()) return 1;

  Schema schema;                         // 建表：student(id INT, name VARCHAR)
  schema.AddColumn("id", ValueType::kInt32);
  schema.AddColumn("name", ValueType::kVarchar, 64);
  (void)storage->create_table("student", schema);

  Record rec;                            // 插一行
  rec.AddValue(Value::Int(1));
  rec.AddValue(Value::Varchar("Alice"));
  Rid rid;
  (void)storage->insert_record("student", rec, &rid);

  std::shared_ptr<TableHeap> heap;       // 查出来
  (void)storage->open_table("student", &heap);
  for (auto it = heap->begin(); it != heap->end(); ++it) {
    std::printf("row: %s\n", it->ToString().c_str());   // [1, Alice]
  }

  storage->Close();                      // 关闭前自动 FlushAllPages
  return 0;
}
```

**运行结果**：`row: [1, Alice]`。完整的建表→插入→扫描→过滤→投影→删除见 `examples/crud_flow.cpp`。

---

## 3. 接口全表

### 3.1 `IStorage`（对外唯一契约面）

| 方法 | 前置条件 | 后置条件 | 返回值 | 失败错误码 | 线程安全 |
| --- | --- | --- | --- | --- | --- |
| `Status Open(const StorageConfig&)` | 未打开 | 已打开，可访问 | `kOk` | `kInvalidConfig` / `kVersionMismatch` / `kCorruptPage` / `kIoError` | 否 |
| `void Close()` | 已打开 | FlushAllPages + 关日志 | — | — | 否 |
| `Status read_page(page_id_t, char*)` | 已打开；buf 为 `page_size` 字节 | buf 填入整页内容 | `kOk` | `kPageNotFound` / `kIoError` | 否 |
| `Status write_page(page_id_t, const char*)` | 已打开 | 整页写入 | `kOk` | `kIoError` | 否 |
| `Page* get_page(page_id_t)` | 已打开 | 返回**已 pin** 的页（配 `unpin_page`） | 非空 | `nullptr`（全 pin / 页不存在） | 否 |
| `Status flush_page(page_id_t)` | 已打开 | 脏页写盘、清脏 | `kOk` | `kPageNotFound` | 否 |
| `Status unpin_page(page_id_t, bool is_dirty)` | 已打开；该页已 pin | pin 计数 -1；归零时变为可淘汰 | `kOk` | `kPageNotFound` | 否 |
| `Status allocate_page(page_id_t* out)` | 已打开 | out 填新页号（已 pin） | `kOk` | `kNoFreeFrame` / `kNoFreePage` | 否 |
| `Status free_page(page_id_t)` | 已打开 | 页进入空闲链表 | `kOk` | `kInvalidArgument` | 否 |
| `Status create_table(name, Schema)` | 已打开；表名未存在 | 表已建立（目录持久化） | `kOk` | `kTableAlreadyExists` / `kNoFreePage` | 否 |
| `Status drop_table(name)` | 表已存在 | 表数据页全部释放 | `kOk` | `kTableNotFound` | 否 |
| `Status open_table(name, shared_ptr<TableHeap>*)` | 表已存在 | out 指向可扫描的表 | `kOk` | `kTableNotFound` | 否 |
| `Status insert_record(table, Record, Rid*)` | 表已存在；Record 列数=schema | out 填 `Rid{page,slot}` | `kOk` | `kRecordTooLarge` / `kNoFreePage` / `kTypeMismatch` | 否 |
| `Status delete_record(table, Rid)` | Rid 有效 | 标记删除（槽 len=0） | `kOk` | `kInvalidArgument` / `kPageNotFound` | 否 |
| `const BufferStats& get_stats() const` | 已打开 | 返回缓冲池统计 | — | — | 否 |
| `std::string dump_stats() const` | 已打开 | 表格化统计文本 | — | — | 否 |
| `std::vector<std::string> recent_evictions() const` | 已打开 | 最近 ≤10 条替换日志 | — | — | 否 |

**线程安全**：全部接口为**单线程设计**。事务 / 并发 / 锁由引擎组在上层自行处理。

### 3.2 工厂

```cpp
// storage_factory.h
std::unique_ptr<IStorage> CreateStorage(const StorageConfig& config);   // 当前实现 FileStorage
```

### 3.3 表访问接口（`table/`）

```cpp
// table_heap.h
class TableHeap {
  Status InsertRecord(const Record&, Rid* out);   // 页满自动分配新页挂链表尾
  Status GetRecord(const Rid&, Record* out);
  Status DeleteRecord(const Rid&);                // 标记删除
  TableIterator begin();  TableIterator end();    // 跨页扫描，跳过墓碑
};
// table_iterator.h
class TableIterator {
  const Record& operator*() const;   // 当前记录（副本，勿跨 ++ 保留引用）
  const Record* operator->() const;
  Rid rid() const;
  bool operator!=(const TableIterator&) const;
  TableIterator& operator++();
};
```

### 3.4 公共类型（`common/`）

| 类型 | 说明 |
| --- | --- |
| `Rid { page_id_t page_id; slot_id_t slot_id; }` | 记录定位（`types.h`） |
| `Schema` / `Column` | 表结构（`schema.h`） |
| `Record` / `Value` | 一行记录 / 单元格值，支持 NULL/INT/VARCHAR 等（`record.h`/`value.h`） |
| `StorageConfig` | 配置（`config.h`） |
| `Status` / `StatusCode` | 统一返回（`status.h`） |
| `BufferStats` | 缓冲池统计（`buffer_stats.h`） |

---

## 4. 典型调用时序（建表 → 插入 → 扫描 → 过滤 → 投影 → 删除）

真实源码见 `examples/crud_flow.cpp`。下面标注**每个算子该调哪个接口**：

```
DDL   建表        → IStorage::create_table("student", schema)
DML   插入        → IStorage::insert_record("student", rec, &rid)     （Insert 算子）
      扫描        → IStorage::open_table("student", &heap)             （SeqScan 算子）
                    + TableHeap::begin()/end() 遍历
      过滤        → 引擎组内存内判断（对扫描结果做谓词，age>24 等）     （Filter 算子）
      投影        → 引擎组内存内取列/取前 N 行                         （Project 算子）
      删除        → IStorage::delete_record("student", rid)            （Delete 算子，标记删除）
生命周期 关闭      → IStorage::Close()（自动 FlushAllPages）
```

> Filter / Project 是引擎组的**内存内运算**，存储系统只提供原始全表扫描，不感知过滤/投影。

---

## 5. 数据类型与错误码对照

### 5.1 数据类型（`ValueType`）

| 枚举 | 存储 | 说明 |
| --- | --- | --- |
| `kNull` | 1 bit（null_bitmap） | 空值 |
| `kBool` | 1 字节 | |
| `kInt32` | 4 字节（小端） | |
| `kInt64` | 8 字节（小端） | |
| `kFloat` / `kDouble` | 4/8 字节 | |
| `kVarchar` / `kChar` | `u16 len` + 字节 | 变长，前缀长度 |
| `kDate` | 预留 | 未实现 |

### 5.2 错误码（`StatusCode`）

| 错误码 | 含义 | 常见触发 |
| --- | --- | --- |
| `kOk` | 成功 | — |
| `kIoError` | 磁盘 I/O 失败 | 文件读写失败 |
| `kInvalidConfig` | 配置非法 | page_size 非 2 的幂 / <512 / >65535；未知策略 |
| `kPageNotFound` | 页不存在 | 读越界页 |
| `kNoFreeFrame` | 无空闲帧 | 缓冲池全部被 pin |
| `kNoFreePage` | 无空闲页 | 磁盘满 |
| `kPageFull` | 页已满 | 槽式布局插不下 |
| `kRecordTooLarge` | 记录超长 | 单条记录 > 页容量 |
| `kInvalidArgument` | 参数非法 | 无效 Rid / 槽号越界 |
| `kCorruptPage` | 页损坏 | MetaPage magic 不匹配 |
| `kVersionMismatch` | 版本不兼容 | 旧数据文件 format_version 不符 |
| `kTableNotFound` | 表不存在 | open/drop/insert 未建表 |
| `kTableAlreadyExists` | 表已存在 | 重复建表 |
| `kTypeMismatch` | 类型不匹配 | Record 列数与 schema 不符 |
| `kNotImplemented` | 未实现 | 目录超单页等 |

---

## 6. 编译与链接

```cmake
# 引擎组 CMakeLists.txt
add_subdirectory(path/to/cella)                        # 或已安装的包
target_link_libraries(your_engine PRIVATE cella_storage)

set(CMAKE_CXX_STANDARD 17)                             # 必须 C++17
```

- **CMake target**：`cella_storage`（静态库）
- **C++ 标准**：C++17（`CMAKE_CXX_STANDARD 17`，`CMAKE_CXX_STANDARD_REQUIRED ON`）
- **include 方式**（扁平名）：
  ```cpp
  #include "i_storage.h"          // api/
  #include "schema.h"             // common/
  #include "table_heap.h"         // table/
  ```
- **零第三方依赖**；MSVC / g++ 均可编译，`-Wall -Wextra -Werror` 零 warning。

---

## 7. 常见坑与注意事项

1. **pin 必须配对 unpin**：`get_page` / `allocate_page` 返回**已 pin** 的页，用后必须 `unpin_page(pid, dirty)`。推荐用 `PageGuard` RAII（内部实现，见 `API.md`）——但引擎组正常数据走**表级接口**，一般无需直接 pin 页。
2. **`get_page` 返回的是被 pin 的页**：别长期持有不 unpin，否则池会耗尽（`kNoFreeFrame`）。
3. **页满时上层如何处理**：`insert_record` 自动分配新页挂链表尾，上层**无需处理页满**；只有单条记录超页才返回 `kRecordTooLarge`。
4. **删除是标记删除，不是物理删除**：`delete_record` 只把槽 `len` 置 0（墓碑），**不回收空间**。空间回收靠 `drop_table` 或将来 B+ 树的页级 `free_page`。
5. **记录不能跨页**：单条记录必须 ≤ `page_size - 页头 - 槽`，否则 `kRecordTooLarge`。
6. **原始字节读写走 `read_page`/`write_page`**（不 pin）；把 catalog 当特殊表存就用这两个 + 预留的 `PageType::kCatalogPage`。
7. **`TableIterator` 返回的 `Record` 是副本**：`*it` 是缓存副本，跨 `++it` 后失效，勿保留引用。
8. **`Close()` 前数据才保证落盘**：`Close()` 会 FlushAllPages + 持久化目录；进程崩溃则最近未 flush 的脏页可能丢失（本模块无 WAL）。
9. **单条 varchar ≤ 65535 字节**（长度前缀是 u16）。

---

## 8. 版本变更记录

| 版本 | 日期 | 变更内容 | 影响方 |
| --- | --- | --- | --- |
| 0.1 | 2026-09-08 | 首版：页式存储 + 缓冲池（LRU/FIFO/CLOCK）+ 统一接口 IStorage + 记录序列化 + 表→页映射 + 持久化 | 引擎组 |
| — | 预留 | B+ 树索引（`PageType::kIndexPage`）、事务、并发 | 引擎组（后续接入点） |
