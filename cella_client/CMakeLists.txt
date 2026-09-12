# ═══════════════════════════════════════════════════════════════════════
#  cella_client —— 图形客户端：内嵌 HTTP 服务（Winsock2，零第三方依赖）
#
#  依赖（静态库，PUBLIC 传递包含目录）：
#     cella_db_core  数据库层（其自身链接 cella_sql_core + cella_storage）
#
#  产物：
#     cella_client_core   静态库（net / api / server 三层）
#     cella_web           服务入口：--data <dir> --port N [--web-dir <dir>]
#     cella_client_tests  客户端层测试（json / http / sql_builder / api 端到端）
#
#  前端资源在 web/（无构建步骤），编译期注入默认路径，运行时 --web-dir 可覆盖。
# ═══════════════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.16)
project(cella_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE CELLA_CLIENT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/cella/client/*.cpp)
file(GLOB_RECURSE CELLA_CLIENT_TEST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)

add_library(cella_client_core STATIC ${CELLA_CLIENT_SOURCES})

target_include_directories(cella_client_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cella_client_core
  PUBLIC
    cella_db_core
)

# socket.cpp 用到 winsock2.h：这两个宏必须在任何 Windows 头之前定义
target_compile_definitions(cella_client_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
  target_link_libraries(cella_client_core PUBLIC ws2_32)
  target_compile_options(cella_client_core PUBLIC /utf-8 PRIVATE /W4 /WX)
else()
  target_compile_options(cella_client_core PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

# ── 服务入口 ─────────────────────────────────────────────────
add_executable(cella_web src/main.cpp)
target_link_libraries(cella_web PRIVATE cella_client_core)
target_compile_definitions(cella_web PRIVATE
  CELLA_CLIENT_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
)
if(MSVC)
  target_compile_options(cella_web PRIVATE /W4 /WX)
else()
  target_compile_options(cella_web PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

# ── 测试 ─────────────────────────────────────────────────────
if(CELLA_CLIENT_TEST_SOURCES)
  add_executable(cella_client_tests
    tests/test_main.cpp
    ${CELLA_CLIENT_TEST_SOURCES}
  )
  target_include_directories(cella_client_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(cella_client_tests PRIVATE cella_client_core)
  target_compile_definitions(cella_client_tests PRIVATE
    CELLA_CLIENT_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
    CELLA_CLIENT_TESTDATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_data"
  )
  if(MSVC)
    target_compile_options(cella_client_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(cella_client_tests PRIVATE -Wall -Wextra -Werror -pedantic)
  endif()
  enable_testing()
  add_test(NAME cella_client_tests COMMAND cella_client_tests)
endif()
