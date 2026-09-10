# TEST_REPORT.md — 测试报告

> 测试框架：`tests/mini_test.h`（TEST_CASE / EXPECT_EQ / EXPECT_TRUE / EXPECT_OK）。
> 运行：`./storage_tests`。**结果：30 用例 / 4620 断言 / 0 失败**（2026-09-08，MSVC Release，零 warning）。

---

## 1. 用例表

| 用例 | 目的 | 输入 | 预期 | 实际 | 结论 |
| --- | --- | --- | --- | --- | --- |
| byte_buffer_little_endian | 小端编解码 | 0x1234/0x12345678/u64 | 字节序 [低→高] | 一致 | ✅ |
| byte_buffer_streaming | ByteBuffer 流式 | 各类 Put/Get + 字符串 | 往返一致 | 一致 | ✅ |
| status_ok_and_error | Status 语义 | 默认/错误 | ok/错误码/消息 | 一致 | ✅ |
| config_validate_page_size | 配置校验 | 512/8192/768/256 | 2 的幂且≥512 | 一致 | ✅ |
| version_constants | 版本常量 | — | "cella-storage/0.1"/1 | 一致 | ✅ |
| disk_offset_and_reopen | 偏移 + 持久化 | 写页 1/2/3 重开 | 字节一致 | 一致 | ✅ |
| disk_offset_formula | 偏移公式 | 只写页 5 | 文件=6×512 | 一致 | ✅ |
| disk_free_list_reuse | 释放复用 | 分配→释放→分配 | 复用同页号、页数不增 | 一致 | ✅ |
| mem_disk_free_list_reuse | 内存后端复用 | 同上 | 复用 | 一致 | ✅ |
| slotted_insert_get_delete | 槽式增删读 | 插 2 删 1 | 墓碑跳过 | 一致 | ✅ |
| slotted_page_full_and_too_large | 页满/超长 | 64B×N / 512B | kPageFull(7条)/kRecordTooLarge | 一致 | ✅ |
| buffer_first_miss_second_hit | 缓存命中 | 2 次访问同页 | MISS→HIT | 一致 | ✅ |
| lru_eviction_order | LRU 淘汰 | 1,2,1,3(pool=2) | 淘汰 2 | 一致 | ✅ |
| fifo_vs_lru_different | 策略差异 | 1,2,3,1,4(pool=3) | LRU 淘汰2、FIFO 淘汰1 | 不同 | ✅ |
| pinned_page_not_evicted | pin 保护 | pin 住页 1 | 不被淘汰 | 一致 | ✅ |
| all_pinned_no_free_frame | 全 pin | pin 满 2 帧 | get_page=nullptr 不死锁 | 一致 | ✅ |
| dirty_page_flushed_before_evict | 脏页先写盘 | 脏页淘汰 | dirty_flush=1 | 一致 | ✅ |
| page_guard_auto_unpin | RAII | guard 析构 | pin 归零可淘汰 | 一致 | ✅ |
| replacer_factory_and_stats_format | 工厂+格式 | — | LRU/FIFO/CLOCK 注册、hit_rate 格式 | 一致 | ✅ |
| clock_replacer_basic | CLOCK 行为 | Insert/Pin/Victim | 引用位二次机会 | 一致 | ✅ |
| replacer_and_stats_log_output | 日志输出 | 淘汰 + LogStats | REPLACER/STATS 行 | 一致 | ✅ |
| serialize_roundtrip_int_varchar_null | 序列化往返 | INT/VARCHAR/NULL | 往返一致 | 一致 | ✅ |
| insert_1000_rows_scan | 跨页扩展+扫描 | 插 1000 行 | 全表扫描 1000 一致 | 一致 | ✅ |
| delete_then_scan_skips | 删除生效 | 删偶数 id | 只剩奇数、计数正确 | 一致 | ✅ |
| record_too_large | 超长记录 | 5000B varchar | kRecordTooLarge | 一致 | ✅ |
| persistence_close_open | 持久化 | 100 行 Close→Open | 数据完整 | 一致 | ✅ |
| version_mismatch | 版本不兼容 | format_version=999 | kVersionMismatch | 一致 | ✅ |
| create_table_errors | 错误码 | 重复建/开不存在表 | kTableAlreadyExists/kTableNotFound | 一致 | ✅ |
| example_quickstart | 文档示例 | 运行 quickstart | 退出 0 + "quickstart OK" | 一致 | ✅ |
| example_crud_flow | 文档示例 | 运行 crud_flow | 退出 0 + "crud OK" | 一致 | ✅ |

---

## 2. 性能数据（`./storage_demo`，MSVC Release，本机）

| 指标 | 数值 |
| --- | --- |
| 插入 5000 行 | 3.4 ms（31 页，约 161 行/页） |
| 全表扫描 5000 行 | < 1 ms |
| LRU 命中率（含重访问负载，pool=3） | 39.50% |
| FIFO 命中率（同负载） | 20.00% |
| CLOCK 命中率（同负载） | 60.00% |

> 命中率对比证明：① 不同替换策略在**同一负载**下结果确实不同；② 该负载下 CLOCK 优于 LRU 优于 FIFO。

---

## 3. 覆盖说明

- **指导书硬性验收**：进程重启数据不丢（`persistence_close_open`）、全 pin 不崩溃（`all_pinned_no_free_frame`）、新增策略零改动（`clock_replacer_basic` + demo ⑪）。
- **文档自验证**：`examples/` 下两个示例均被 `test_examples.cpp` 编译 + 运行 + 输出核对通过。
- **未覆盖**（预留，非本次范围）：B+ 树、事务、并发、WAL 崩溃恢复。
