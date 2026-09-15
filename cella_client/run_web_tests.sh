#!/usr/bin/env bash
# run_web_tests.sh —— 前端自测总入口（无 npm / 无第三方依赖）。
#
#   ./run_web_tests.sh [端口]
#
# 做三件事：
#   ① 起一个临时数据的 cella_web.exe（进程在退出时收掉）
#   ② 用无头 Chrome 依次打开 web/_selftest/*.html，把页面里 <pre id="out"> 的
#      断言结果抠出来统计
#   ③ 打一张汇总表；有任何 FAIL 就以非 0 退出（可直接接 CI）
#
# 两个必须踩过的坑（都已在本脚本里处理）：
#   * **不要从脚本源码里数 PASS/FAIL** —— 套件页面自己也写着 'PASS '/'FAIL ' 字面量，
#     整份 DOM 一 grep 就会把源码算进去，出现「54 PASS 2 FAIL」这种假结果。
#     只统计 <pre id="out"> 区块内的行。
#   * headless 下必须带 `--enable-logging=stderr`。模块图一旦坏掉（例如 app.js
#     引用了没 import 的符号），页面只会静静地不启动 —— 断言全挂却看不出原因；
#     这个开关会把 "Uncaught ReferenceError: ..." 打到日志里，一眼定位。
#
# 环境变量：
#   CHROME=...   指定浏览器可执行文件（默认自动探测 Chrome/Edge）
#   PORT=...     监听端口（默认 8131）

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 本脚本放在 cella_client/ 下，但构建产物在仓库根的 build/ —— 往上找一层，
# 谁先有 build/cella_client/cella_web.exe 谁就是根。
if [ -x "$SCRIPT_DIR/build/cella_client/cella_web.exe" ]; then
  ROOT="$SCRIPT_DIR"
elif [ -x "$SCRIPT_DIR/../build/cella_client/cella_web.exe" ]; then
  ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
else
  echo "找不到 build/cella_client/cella_web.exe，请先在仓库根跑一次构建" >&2
  exit 2
fi
PORT="${1:-${PORT:-8131}}"
DATA="$ROOT/build/web_test_data"
LOG="$ROOT/build/web_test_server.log"
OUTDIR="$ROOT/build/web_tests"
SELFTEST="$SCRIPT_DIR/web/_selftest"
SUITES="grid editor panels layout app designer"

find_browser() {
  if [ -n "${CHROME:-}" ]; then
    # 允许从 PowerShell 等外部传进来「Windows 形态」的路径（C:\...\chrome.exe）：
    # Git Bash 里直接执行反斜杠路径不可靠，先换成 POSIX 形态。
    if command -v cygpath > /dev/null 2>&1; then
      cygpath -u "$CHROME" 2>/dev/null || echo "$CHROME"
    else
      echo "$CHROME"
    fi
    return
  fi
  for p in \
    "/c/Program Files/Google/Chrome/Application/chrome.exe" \
    "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
    "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" \
    "/c/Program Files/Microsoft/Edge/Application/msedge.exe"; do
    [ -f "$p" ] && { echo "$p"; return; }
  done
  echo ""
}

BROWSER="$(find_browser)"
if [ -z "$BROWSER" ]; then
  echo "找不到浏览器：请用 CHROME=/path/to/chrome 指定" >&2
  exit 2
fi

mkdir -p "$OUTDIR"
rm -rf "$DATA"

echo "=== 启动 cella_web（data=$DATA port=$PORT）==="
"$ROOT/build/cella_client/cella_web.exe" --data "$DATA" --port "$PORT" > "$LOG" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT

# 等端口真的开始服务（curl 要绕开环境里的代理，否则连 127.0.0.1 也会被拦）
UP=0
for _ in $(seq 1 30); do
  if curl -s -m 2 --noproxy '*' "http://127.0.0.1:$PORT/api/health" > /dev/null 2>&1; then UP=1; break; fi
  sleep 0.5
done
if [ "$UP" -ne 1 ]; then
  echo "服务未就绪，日志：" >&2; cat "$LOG" >&2; exit 1
fi

TOTAL_PASS=0; TOTAL_FAIL=0; BAD=""
printf '\n%-12s %6s %6s   %s\n' "套件" "PASS" "FAIL" "备注"
printf '%s\n' "------------------------------------------------------------"

for s in $SUITES; do
  page="$SELFTEST/$s.html"
  [ -f "$page" ] || { printf '%-12s %6s %6s   %s\n' "$s" "-" "-" "页面不存在，跳过"; continue; }
  dom="$OUTDIR/$s.dom.html"
  err="$OUTDIR/$s.err.log"
  rm -rf "$OUTDIR/profile_$s"
  "$BROWSER" --headless=new --disable-gpu --no-first-run --no-proxy-server \
    --hide-scrollbars --enable-logging=stderr --v=0 \
    --virtual-time-budget=120000 \
    --user-data-dir="$(cygpath -w "$OUTDIR/profile_$s" 2>/dev/null || echo "$OUTDIR/profile_$s")" \
    --dump-dom "http://127.0.0.1:$PORT/_selftest/$s.html" > "$dom" 2> "$err"

  # 只取结果区块，避免把套件源码里的 'PASS '/'FAIL ' 字面量算进来
  sed -n '/<pre id="out">/,/<\/pre>/p' "$dom" | sed 's/<[^>]*>//g' > "$OUTDIR/$s.out.txt"
  p=$(grep -c '^PASS' "$OUTDIR/$s.out.txt")
  f=$(grep -c '^FAIL' "$OUTDIR/$s.out.txt")

  note=""
  if [ "$p" -eq 0 ] && [ "$f" -eq 0 ]; then
    note="无断言输出（页面可能没跑起来）"
  elif [ "$f" -gt 0 ]; then
    note="失败：$(grep '^FAIL' "$OUTDIR/$s.out.txt" | head -1 | cut -c1-90)"
  fi
  # 控制台异常单独提示：断言通过也可能有被吞掉的报错
  if grep -qi "Uncaught\|ReferenceError\|TypeError" "$err" 2>/dev/null; then
    note="${note:+$note; }控制台有异常（见 $err）"
  fi

  printf '%-12s %6s %6s   %s\n' "$s" "$p" "$f" "$note"
  TOTAL_PASS=$((TOTAL_PASS + p)); TOTAL_FAIL=$((TOTAL_FAIL + f))
  [ "$f" -gt 0 ] && BAD="$BAD $s"
done

printf '%s\n' "------------------------------------------------------------"
printf '前端自测：通过 %d / 失败 %d\n' "$TOTAL_PASS" "$TOTAL_FAIL"
if [ -n "$BAD" ]; then
  echo "失败套件：$BAD"
  exit 1
fi
[ "$TOTAL_PASS" -eq 0 ] && { echo "一条断言都没跑到，视为失败"; exit 1; }
echo "全部通过。"
