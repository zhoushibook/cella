# ═══════════════════════════════════════════════════════════════════════
#  cella_db —— 整合层：执行调度 + 数据库层（目录/记录/事务/并发）+ CLI
#
#  依赖（均为静态库，PUBLIC 传递包含目录）：
#     cella_sql_core  编译器前端（SQL 文本 → AST/计划）
#     cella_storage   存储引擎（页/缓冲池/表堆）
# ═══════════════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.16)
project(cella_db LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CONFIGURE_DEPENDS：新增/删除源文件后重新配置时自动刷新列表
file(GLOB_RECURSE CELLA_DB_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/cella/db/*.cpp)
file(GLOB_RECURSE CELLA_DB_TEST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)

add_library(cella_db_core STATIC ${CELLA_DB_SOURCES})

target_include_directories(cella_db_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # 存储层包含根：cella_storage 目标只导出 api/common/table 三个子目录（裸名包含），
    # 整合层统一使用 "cella/storage/..." 形式，故补上其 include 根。
    ${CMAKE_SOURCE_DIR}/cella_storage/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cella_db_core
  PUBLIC
    cella_sql_core
    cella_storage
)

if(MSVC)
  target_compile_options(cella_db_core PUBLIC /utf-8 PRIVATE /W4 /WX)
else()
  target_compile_options(cella_db_core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

# ── 统一 CLI（完整系统入口）──────────────────────────────────
add_executable(cella_db src/main.cpp)
target_link_libraries(cella_db PRIVATE cella_db_core)

# ── 测试 ────────────────────────────────────────────────────
if(CELLA_DB_TEST_SOURCES)
  add_executable(cella_db_tests
    tests/test_main.cpp
    ${CELLA_DB_TEST_SOURCES}
  )
  target_include_directories(cella_db_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(cella_db_tests PRIVATE cella_db_core)
  target_compile_definitions(cella_db_tests PRIVATE
    # 编译器自带的测试 SQL 与 golden 目录（相对本模块定位，不依赖聚合工程布局）
    CELLA_DB_SQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../cella_sql/tests/sql"
    CELLA_DB_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sql"
    CELLA_DB_TESTDATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_data"
  )
  enable_testing()
  add_test(NAME cella_db_tests COMMAND cella_db_tests)
endif()

# ── 全链路演示 ───────────────────────────────────────────────
file(GLOB CELLA_DB_EXAMPLES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
foreach(EXAMPLE_SRC ${CELLA_DB_EXAMPLES})
  get_filename_component(EXAMPLE_NAME ${EXAMPLE_SRC} NAME_WE)
  add_executable(${EXAMPLE_NAME} ${EXAMPLE_SRC})
  target_link_libraries(${EXAMPLE_NAME} PRIVATE cella_db_core)
  target_compile_definitions(${EXAMPLE_NAME} PRIVATE
    CELLA_DB_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sql"
    CELLA_DB_DEMO_DIR="${CMAKE_CURRENT_BINARY_DIR}/demo_data")
endforeach()
