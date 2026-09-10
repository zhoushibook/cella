# ═══════════════════════════════════════════════════════════════════════
#  cella 数据库系统 —— 顶层聚合工程
#
#  三个子模块（各自保持独立可构建，也可在此一起构建）：
#
#    cella_sql             SQL 编译器前端  → 静态库 cella_sql_core + CLI cella_sql
#    cella_storage         页式存储/缓冲池 → 静态库 cella_storage  + demo/tests
#    cella_db              【本次整合新增】执行调度 + 数据库层 + 事务/并发 + 统一 CLI
#
#  构建：
#      cmake -S . -B build -G Ninja
#      cmake --build build
#  产物：build/cella_db/cella_db.exe（完整系统）、build/cella_sql/cella_sql.exe、
#        build/cella_storage/storage_demo.exe
# ═══════════════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.16)
project(cella_db_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_subdirectory(cella_storage)
add_subdirectory(cella_sql)
add_subdirectory(cella_db)
