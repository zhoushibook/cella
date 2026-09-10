# cella SQL 编译器 —— CMake 构建脚本
#
# 目录内两个目标：
#   cella_sql_core  静态库：词法/语法/语义/计划/优化/打印（供 cella_db 复用，编译器前端内核）
#   cella_sql       可执行：原 CLI（行为与输出与整合前完全一致）
cmake_minimum_required(VERSION 3.16)
project(cella_sql LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 编译器前端内核（静态库）─────────────────────────────────
# include/ 目录设为 PUBLIC：整合层（cella_db）据此以 "cella/cella_ast.h" 形式包含。
add_library(cella_sql_core STATIC
    src/cella_lexer.cpp
    src/cella_parser.cpp
    src/cella_semantic.cpp
    src/cella_planner.cpp
    src/cella_optimizer.cpp
    src/cella_printer.cpp
)

target_include_directories(cella_sql_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    # 源码统一 UTF-8
    target_compile_options(cella_sql_core PUBLIC /utf-8 PRIVATE /W3)
else()
    target_compile_options(cella_sql_core PRIVATE -Wall -Wextra)
endif()

# ── 原 CLI 可执行目标 ───────────────────────────────────────
add_executable(cella_sql src/main.cpp)
target_link_libraries(cella_sql PRIVATE cella_sql_core)
