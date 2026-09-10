# API.md — 完整接口参考

> 读者：引擎组（对外契约）+ 本模块后续维护者（内部实现）。
> 对外部分见 `INTERFACE_CONTRACT.md`，本文补充**签名细节与内部实现者信息**。

---

## 1. 对外契约面（`api/` + `common/` + `table/`）

### 1.1 `IStorage`（`api/i_storage.h`）

```cpp
namespace cella::storage {
class IStorage {
 public:
  static constexpr const char* kInterfaceVersion = "cella-storage/0.1";
  virtual ~IStorage() = default;
  virtual const char* InterfaceVersion() const { return kInterfaceVersion; }

  virtual Status Open(const StorageConfig&) = 0;
  virtual void   Close() = 0;

  virtual Status read_page(page_id_t, char*) = 0;
  virtual Status write_page(page_id_t, const char*) = 0;
  virtual Page*  get_page(page_id_t) = 0;
  virtual Status flush_page(page_id_t) = 0;
  virtual Status unpin_page(page_id_t, bool is_dirty) = 0;
  virtual Status allocate_page(page_id_t* out) = 0;
  virtual Status free_page(page_id_t) = 0;

  virtual Status create_table(const std::string&, const Schema&) = 0;
  virtual Status drop_table(const std::string&) = 0;
  virtual Status open_table(const std::string&, std::shared_ptr<TableHeap>*) = 0;
  virtual Status insert_record(const std::string&, const Record&, Rid*) = 0;
  virtual Status delete_record(const std::string&, const Rid&) = 0;

  virtual const BufferStats& get_stats() const = 0;
  virtual std::string dump_stats() const = 0;
  virtual std::vector<std::string> recent_evictions() const = 0;
};
std::unique_ptr<IStorage> CreateStorage(const StorageConfig&);   // storage_factory.h
}
```

**实现**：`FileStorage`（`api/file_storage.h`）——文件磁盘后端 + 缓冲池 + 表目录。

### 1.2 公共类型

```cpp
// types.h
using page_id_t  = uint32_t;  using slot_id_t = uint16_t;  using frame_id_t = uint32_t;
constexpr page_id_t kInvalidPageId = 0xFFFFFFFFu;  constexpr slot_id_t kInvalidSlotId = 0xFFFFu;
constexpr page_id_t kMetaPageId = 0u;
enum class PageType : uint16_t { kMetaPage, kDataPage, kCatalogPage, kIndexPage, kFreeListPage, kOverflowPage };
struct Rid { page_id_t page_id; slot_id_t slot_id; bool IsValid() const; };
enum class ValueType : uint8_t { kNull, kBool, kInt32, kInt64, kFloat, kDouble, kVarchar, kChar, kDate };

// status.h
enum class StatusCode : uint8_t { kOk, kIoError, kInvalidConfig, kPageNotFound, kNoFreeFrame,
  kNoFreePage, kPageFull, kRecordTooLarge, kInvalidArgument, kCorruptPage, kVersionMismatch,
  kTableNotFound, kTableAlreadyExists, kTypeMismatch, kNotImplemented };
class [[nodiscard]] Status {           // 忽略返回值 → 编译警告（-Werror 即报错）
  bool ok() const; StatusCode code() const; const std::string& message() const;
  static Status OK(); static Status Error(StatusCode, std::string msg = "");
};

// config.h
enum class LogLevel : uint8_t { kDebug, kInfo, kWarn, kError };
struct StorageConfig {
  uint32_t page_size = 4096;   size_t pool_size = 64;   std::string replacer = "LRU";
  std::string data_dir = "./data";  std::string db_file = "cella.db";
  bool enable_log = true;  std::string log_path = "./data/cella-storage.log";
  bool enable_checksum = false;  LogLevel log_level = LogLevel::kInfo;
  Status Validate() const;  std::string ToString() const;
};

// value.h / schema.h / record.h
struct Value { ValueType type; bool bool_val; int32_t int32_val; int64_t int64_val;
  float float_val; double double_val; std::string str_val;
  static Value Null()/Bool()/Int()/BigInt()/Float()/Double()/Varchar(); bool IsNull() const; };
struct Column { std::string name; ValueType type; uint16_t max_len; };
class Schema { void AddColumn(name, type, max_len=0); size_t column_count() const; const Column& column(i) const; };
class Record { void AddValue(const Value&); size_t value_count() const; const Value& value(i) const; };

// buffer_stats.h
struct BufferStats { uint64_t access, hit, miss, evict, dirty_flush, disk_reads, disk_writes,
  page_allocs, page_frees; double hit_rate() const; std::string ToLogLine() const; std::string ToString() const; };
```

---

## 2. 内部实现接口（`disk/ page/ buffer/ record/ table/`，不导出）

> 以下仅供本模块维护者参考，引擎组 include 不到。

### 2.1 磁盘层 `disk/`

```cpp
class IDiskManager {
  virtual Status Open(const std::string& path) = 0;      // 读 MetaPage，校验 magic/version
  virtual Status Close() = 0;
  virtual Status ReadPage(page_id_t, char* data) = 0;    // offset = page_id * page_size
  virtual Status WritePage(page_id_t, const char*) = 0;
  virtual page_id_t AllocatePage() = 0;                  // 优先复用空闲页链表；失败 kInvalidPageId
  virtual Status FreePage(page_id_t) = 0;
  virtual uint64_t GetDiskReadCount() const = 0;
  virtual uint64_t GetDiskWriteCount() const = 0;
  virtual uint64_t GetPageCount() const = 0;
  virtual uint32_t GetPageSize() const = 0;
  virtual page_id_t GetCatalogRootPage() const = 0;      // MetaPage.catalog_root_page
  virtual Status SetCatalogRootPage(page_id_t) = 0;
};
// 实现：FileDiskManager（文件）、MemDiskManager（测试，行为一致）
```

### 2.2 页层 `page/`

```cpp
class Page {                          // vector<char>，支持可配置页大小（约束 #2）
  char* data(); uint32_t page_size() const; page_id_t page_id() const;
  void set_page_id(page_id_t); void reset_memory();
  // 页头读写（小端）：page_id/page_type/prev/next/slot_count/free_end/checksum
};
constexpr size_t kPageHeaderSize = 32;
namespace page_header { /* 各字段偏移 */ }

class PageLayout {                    // 页布局抽象
  virtual Status InsertRecord(const char*, uint16_t len, slot_id_t* out) = 0;
  virtual Status GetRecord(slot_id_t, const char** data, uint16_t* len) = 0;
  virtual Status DeleteRecord(slot_id_t) = 0;   // 标记删除
  virtual uint16_t GetFreeSpace() const = 0;
};
class SlottedPageLayout : public PageLayout { explicit SlottedPageLayout(Page*); };

struct MetaPage {                     // page_id=0，32B：magic/version/page_size/page_count/free_list_head/catalog_root_page/checksum
  void Encode(char*) const; static Status Decode(const char*, MetaPage* out);
};
```

### 2.3 缓冲池 `buffer/`

```cpp
class Replacer {                      // 淘汰策略抽象（最重要扩展点）
  virtual void Insert(frame_id_t) = 0;       // pin 归零 → 可淘汰
  virtual bool Pin(frame_id_t) = 0;          // 移出淘汰候选
  virtual bool Victim(frame_id_t* out) = 0;  // 选牺牲帧
  virtual void Remove(frame_id_t) = 0;
  virtual size_t Size() const = 0;
  virtual std::string Name() const = 0;
};
// 实现：LruReplacer / FifoReplacer / ClockReplacer
class ReplacerFactory {
  using Creator = std::function<std::unique_ptr<Replacer>(size_t)>;
  static bool Register(name, creator); static std::unique_ptr<Replacer> Create(name, n);
  static std::vector<std::string> RegisteredNames();
};

class BufferPoolManager {
  BufferPoolManager(size_t pool_size, std::unique_ptr<IDiskManager>, std::unique_ptr<Replacer>);
  Page* get_page(page_id_t);            // 返回已 pin 的页；全 pin 返回 nullptr
  bool  flush_page(page_id_t);          // 脏页才写盘
  Page* NewPage(page_id_t* out);        // 分配新页并 pin
  bool  UnpinPage(page_id_t, bool is_dirty);
  Status read_page(page_id_t, char*);   // 不 pin，整页读（缓存命中则不落盘）
  Status write_page(page_id_t, const char*);  // 不 pin，整页写
  void  FlushAllPages();
  const BufferStats& GetStats() const;
  void SetLogger(ILogger*); void LogStats();
  const std::deque<std::string>& recent_evictions() const;
};

class PageGuard {                       // RAII：析构自动 Unpin
  PageGuard(BufferPoolManager*, Page*);
  Page* operator->() const; void MarkDirty();
};
```

### 2.4 记录序列化 `record/`

```cpp
class IRecordSerializer {              // 抽象
  virtual Status Serialize(const Record&, const Schema&, std::vector<char>* out) = 0;
  virtual Status Deserialize(const char* data, size_t len, const Schema&, Record* out) = 0;
};
class SlottedRecordSerializer : public IRecordSerializer {};   // §8 格式
```

### 2.5 表 `table/`

```cpp
class TableHeap {                      // 一张表 = 数据页双向链表
  TableHeap(BufferPoolManager*, page_id_t first_page, const Schema&, IRecordSerializer*);
  Status InsertRecord(const Record&, Rid* out);   // 页满自动分配新页挂尾
  Status GetRecord(const Rid&, Record* out);
  Status DeleteRecord(const Rid&);                // 标记删除
  TableIterator begin(); TableIterator end();
};
class FreeSpaceManager { page_id_t hint_page() const; void update_hint(page_id_t); };
```

### 2.6 集成 `integration/`

```cpp
// plan_bridge.h — 演示算子 → 接口映射（stub）
Status SeqScan(IStorage*, table, emit);   // open_table + begin/end
Status Insert(IStorage*, table, rec, out); // insert_record
Status Delete(IStorage*, table, rid);      // delete_record
```

---

## 3. 二进制格式（§8）

- **页偏移**：`offset(page_id) = page_id * page_size`，`page_id=0` 为 MetaPage。
- **数据页**（槽式）：`PageHeader(32B) | 槽数组(向后，每项4B{offset:u16,len:u16}) | 空闲 | 记录(向前)`；`len==0`=墓碑。
- **MetaPage(32B)**：`magic "CELLADB1"(8B) | format_version:u32 | page_size:u32 | page_count:u32 | free_list_head:u32 | catalog_root_page:u32 | checksum:u32`。
- **记录**：`col_count:u16 | null_bitmap:ceil(n/8) | 列数据`，VARCHAR 前缀 `len:u16`。
- **空闲页链表**：复用页头 `next_page_id` 串链，头指针在 MetaPage。

---

## 4. 版本

- 接口版本 `cella-storage/0.1`；数据文件 `format_version = 1`（写入 MetaPage）。
- 出现「库版本与文档不一致」或「旧数据文件」时，`InterfaceVersion()` / `kVersionMismatch` 立即定位。
