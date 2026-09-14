#!/usr/bin/env bash
# build_vc.sh —— build_vc.bat 的 bash 版：同样的 MSVC/SDK 环境，但可从 bash 直接调用。
#
# 为什么需要它：在本会话的 Git Bash 里 `cmd //c build_vc.bat` 会打开交互式 shell
# 而不是执行批处理（表现：vc_build.log 时间戳不变、编译产物不更新）。
# 直接用 bash 设置环境并调用 cmake 则稳定可靠。
#
# 用法： ./build_vc.sh [target]
#
# 注意：CMake 的源文件列表用的是 CONFIGURE_DEPENDS + GLOB，
# 「新增/删除 .cpp」必须重新配置才能生效。cmake --build 在大多数情况下会
# 自动重跑配置，但不可靠（本会话实测：新增 tests/test_*.cpp 后没被纳入）。
# 因此这里**每次先显式 configure 一遍**（0.4s，可以忽略），保证 glob 最新。
set -u

VCTOOLS="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808"
SDK="C:/Program Files (x86)/Windows Kits/10"
SDKVER="10.0.26100.0"
ROOT="C:/Users/Lenovo/Desktop/DBMS/cella"
BUILD="$ROOT/build"
CMAKE="C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
LOG="$BUILD/vc_build.log"

export INCLUDE="$VCTOOLS/include;$SDK/Include/$SDKVER/ucrt;$SDK/Include/$SDKVER/um;$SDK/Include/$SDKVER/shared;$SDK/Include/$SDKVER/winrt;$SDK/Include/$SDKVER/cppwinrt"
export LIB="$VCTOOLS/lib/x64;$SDK/Lib/$SDKVER/ucrt/x64;$SDK/Lib/$SDKVER/um/x64"
export PATH="$VCTOOLS/bin/Hostx64/x64:$PATH"

# ① 先重新配置（刷新 GLOB 到的源文件列表）
"$CMAKE" -S "$ROOT" -B "$BUILD" > "$BUILD/reconf.log" 2>&1
cfg=$?
if [ $cfg -ne 0 ]; then
  echo "CONFIGURE FAILED (EXIT $cfg)"
  tail -20 "$BUILD/reconf.log"
  exit $cfg
fi

# ② 再编译
if [ $# -eq 0 ]; then
  "$CMAKE" --build "$BUILD" > "$LOG" 2>&1
else
  "$CMAKE" --build "$BUILD" --target "$1" > "$LOG" 2>&1
fi
rc=$?
echo "EXIT $rc"
tail -5 "$LOG"
exit $rc
