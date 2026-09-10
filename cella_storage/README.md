# cella_storage — 存储系统模块

数据库引擎的**物理底座**：页式存储 + 缓冲池 + 统一访问接口 `IStorage`。C++17，零第三方依赖，MSVC / g++ 均可编译。

## 快速开始

```bash
# Windows（MSVC，需 Developer Command Prompt 或 vcvars64）
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=cl
cmake --build build

build/storage_demo.exe      # 完整演示（建表→插5000行→扫描→过滤→投影→删除→LRU/FIFO对比→重启→CLOCK）
build/storage_tests.exe     # 单元测试（30 用例 / 4620 断言）
build/quickstart.exe        # 30 秒上手示例
build/crud_flow.exe         # 全流程示例
```

（本机 MSBuild 生成器会崩溃，已改用 Ninja；构建脚本见 `scripts/build_msvc.bat`。）

## 目录

```
include/cella/storage/   api/ common/ table/        ← 对外导出
                         disk/ page/ buffer/ record/ integration/  ← 内部实现
src/cella/storage/       与 include 对应的 .cpp
examples/                quickstart.cpp crud_flow.cpp（文档示例真实源码）
demo/                    storage_demo.cpp
tests/                   mini_test.h + test_*.cpp
docs/                    INTERFACE_CONTRACT.md API.md DESIGN.md EXTENSIBILITY.md TEST_REPORT.md
```

## 文档

| 文档 | 内容 | 读者 |
| --- | --- | --- |
| [INTERFACE_CONTRACT.md](docs/INTERFACE_CONTRACT.md) | 对外契约（30 秒上手 / 接口全表 / 调用时序 / 坑） | 引擎组 |
| [API.md](docs/API.md) | 完整接口参考（含内部实现） | 引擎组 + 维护者 |
| [DESIGN.md](docs/DESIGN.md) | 架构 / 页结构 / 淘汰流程 / InnoDB 对照 | 评审 |
| [EXTENSIBILITY.md](docs/EXTENSIBILITY.md) | 六大扩展点 + 新增策略示例 | 维护者 |
| [TEST_REPORT.md](docs/TEST_REPORT.md) | 用例表 + 性能数据 | 评审 |

## 接口版本

`cella-storage/0.1`（`IStorage::InterfaceVersion()`）；数据文件 `format_version = 1`。
