cmake_minimum_required(VERSION 3.16)
project(cella_storage CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── 编译选项：零 warning（约束 #7）──
if(MSVC)
  add_compile_options(/W4 /WX /utf-8 /wd4127)  # C4127：测试宏比较常量，属噪音
else()
  add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

# ── 存储系统静态库 ───────────────────────────────────────────
# 对外只导出 api/ 与 common/（约束 #2.2 单一契约面）；
# 内部目录 disk/page/buffer/record/table 仅 PRIVATE 可见。
add_library(cella_storage STATIC
  src/cella/storage/common/status.cpp
  src/cella/storage/common/config.cpp
  src/cella/storage/common/logger.cpp
  src/cella/storage/common/types.cpp
  src/cella/storage/common/value.cpp
  src/cella/storage/common/schema.cpp
  src/cella/storage/common/record.cpp
  src/cella/storage/common/buffer_stats.cpp
  src/cella/storage/disk/file_disk_manager.cpp
  src/cella/storage/disk/mem_disk_manager.cpp
  src/cella/storage/page/page.cpp
  src/cella/storage/page/meta_page.cpp
  src/cella/storage/page/slotted_page_layout.cpp
  src/cella/storage/buffer/lru_replacer.cpp
  src/cella/storage/buffer/fifo_replacer.cpp
  src/cella/storage/buffer/clock_replacer.cpp
  src/cella/storage/buffer/replacer_factory.cpp
  src/cella/storage/buffer/buffer_pool_manager.cpp
  src/cella/storage/record/slotted_record_serializer.cpp
  src/cella/storage/index/index_key.cpp
  src/cella/storage/index/b_plus_tree.cpp
  src/cella/storage/table/table_heap.cpp
  src/cella/storage/table/table_iterator.cpp
  src/cella/storage/api/file_storage.cpp
)

target_include_directories(cella_storage
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cella/storage/api
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cella/storage/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cella/storage/table   # 表访问接口（TableHeap/TableIterator）
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── 单元测试 ────────────────────────────────────────────────
add_executable(storage_tests
  tests/test_main.cpp
  tests/test_common.cpp
  tests/test_disk_page.cpp
  tests/test_buffer.cpp
  tests/test_record_table.cpp
  tests/test_persistence.cpp
  tests/test_index.cpp
  tests/test_examples.cpp
)
target_include_directories(storage_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}/include   # 白盒测试可访问内部头
)
target_link_libraries(storage_tests PRIVATE cella_storage)

# ── examples（文档示例的真实源码，必须编译运行通过）───────────
add_executable(quickstart examples/quickstart.cpp)
target_link_libraries(quickstart PRIVATE cella_storage)

add_executable(crud_flow examples/crud_flow.cpp)
target_link_libraries(crud_flow PRIVATE cella_storage)

# test_examples 通过宏拿到示例绝对路径（正向斜杠，避免转义问题）
# 用 CMAKE_CURRENT_BINARY_DIR 而非 CMAKE_BINARY_DIR：后者只在「本工程为顶层工程」时
# 才等于可执行文件所在目录；被 add_subdirectory 嵌套构建时（如 cella 聚合工程）会指向
# 顶层 build 目录而找不到 quickstart.exe / crud_flow.exe。
target_compile_definitions(storage_tests PRIVATE
  QUICKSTART_PATH="${CMAKE_CURRENT_BINARY_DIR}/quickstart.exe"
  CRUD_FLOW_PATH="${CMAKE_CURRENT_BINARY_DIR}/crud_flow.exe"
)

# ── demo ───────────────────────────────────────────────────
add_executable(storage_demo demo/storage_demo.cpp)
target_link_libraries(storage_demo PRIVATE cella_storage)

# ── 诊断程序（白盒，访问内部头）──────────────────────────────
add_executable(bptree_diag diag/bptree_diag.cpp)
target_include_directories(bptree_diag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bptree_diag PRIVATE cella_storage)

add_executable(bptree_diag2 diag/bptree_diag2.cpp)
target_include_directories(bptree_diag2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bptree_diag2 PRIVATE cella_storage)

enable_testing()
add_test(NAME storage_tests COMMAND storage_tests)
