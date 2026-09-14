#!/usr/bin/env bash
# cmbuild.sh -- configure + build cella from bash using the MSVC toolchain.
#
# Why this exists: the host Agent blocks Start-Process / cmd.exe from the
# PowerShell tool, so build.ps1 (which shells out to cmake) cannot run there.
# bash is not restricted that way, but bash does not know about MSVC. So we:
#   1. read build/vcenv.json (dumped by dump_vcenv.ps1 via the VS DevShell),
#   2. export INCLUDE / LIB / LIBPATH / PATH from it, and
#   3. append the Windows SDK include+lib dirs that the DevShell omitted.
#
# Usage:
#   ./cmbuild.sh                       # configure (if needed) + build all
#   ./cmbuild.sh -t cella_storage      # build one target
#   ./cmbuild.sh -r -t cella_storage   # force reconfigure first

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"

VS="C:/Program Files/Microsoft Visual Studio/2022/Community"
CMAKE="$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA_DIR="$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"

RECONFIG=0
TARGET=""
while [ $# -gt 0 ]; do
  case "$1" in
    -r|--reconfigure) RECONFIG=1; shift ;;
    -t|--target) TARGET="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# ---- load the DevShell environment captured by dump_vcenv.ps1 ----
VCENV="$BUILD_DIR/vcenv.json"
if [ ! -f "$VCENV" ]; then
  echo "ERROR: $VCENV not found. Run dump_vcenv.ps1 first." >&2
  exit 1
fi
# Convert JSON to shell exports via python (paths use Windows separators;
# translate to POSIX so bash can use them in PATH/INCLUDE).
# Convert JSON to shell exports via python. The JSON path is passed in
# Windows form (C:\...) because that is what the dump script saw; python here
# is a native Windows binary, so give it the Windows path, not the POSIX one.
VCENV_WIN="$(cygpath -w "$VCENV" 2>/dev/null || echo "$VCENV")"
eval "$(python - "$VCENV_WIN" <<'PY'
import json, sys
p = sys.argv[1]
# strip a UTF-8 BOM if present
raw = open(p, 'rb').read().decode('utf-8-sig')
d = json.loads(raw)
def to_posix(v):
    if not v:
        return ""
    return v.replace("\\", "/")
print('export VC_PATH="%s"'     % to_posix(d.get("PATH", "")))
print('export VC_INCLUDE="%s"'  % to_posix(d.get("INCLUDE", "")))
print('export VC_LIB="%s"'      % to_posix(d.get("LIB", "")))
print('export VC_LIBPATH="%s"'  % to_posix(d.get("LIBPATH", "")))
PY
)"

if [ -z "$VC_PATH" ]; then
  echo "ERROR: vcenv.json had an empty PATH (DevShell did not initialise)." >&2
  exit 1
fi

# ---- locate the newest Windows SDK ----
SDK_INC=""
SDK_LIB=""
for BASE in "/c/Program Files (x86)/Windows Kits/10" "/c/Program Files/Windows Kits/10"; do
  if [ -d "$BASE/Include" ]; then
    VER="$(ls "$BASE/Include" | sort -V | tail -1)"
    if [ -n "$VER" ]; then
      SDK_INC="$BASE/Include/$VER"
      SDK_LIB="$BASE/Lib/$VER"
      break
    fi
  fi
done

# A cl.exe run from bash needs Windows-style separators in INCLUDE/LIB,
# but the PATH entries must stay POSIX for bash to find cl.exe/cmake.
export PATH="$VC_PATH:$NINJA_DIR:$PATH"

WINC_INC=""
WINC_LIB=""
if [ -n "$SDK_INC" ]; then
  for d in ucrt um shared winrt cppwinrt; do
    [ -d "$SDK_INC/$d" ] && WINC_INC="$WINC_INC;C:\\Program Files (x86)\\Windows Kits\\10\\Include\\$(basename "$SDK_INC")\\$(echo "$d" | tr '/' '\\')"
  done
  for d in ucrt/x64 um/x64; do
    [ -d "$SDK_LIB/$d" ] && WINC_LIB="$WINC_LIB;C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\$(basename "$SDK_LIB")\\$(echo "$d" | tr '/' '\\')"
  done
fi

export INCLUDE="$VC_INCLUDE$WINC_INC"
export LIB="$VC_LIB$WINC_LIB"
export LIBPATH="$VC_LIBPATH"

echo "=== cella cmbuild ==="
echo "cmake : $CMAKE"
echo "sdk   : ${SDK_INC:-<none>}"
echo "target: ${TARGET:-<all>}"

# cmake.exe is a native Windows binary: hand it Windows paths, otherwise it
# reads the POSIX "/c/..." form as a relative-ish path and rejects it.
ROOT_WIN="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
BUILD_WIN="$(cygpath -w "$BUILD_DIR" 2>/dev/null || echo "$BUILD_DIR")"

if [ "$RECONFIG" = "1" ] || [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "--- configure ---"
  "$CMAKE" -S "$ROOT_WIN" -B "$BUILD_WIN" -G Ninja \
           -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit 1
fi

echo "--- build ---"
if [ -n "$TARGET" ]; then
  "$CMAKE" --build "$BUILD_WIN" --target "$TARGET"
else
  "$CMAKE" --build "$BUILD_WIN"
fi
