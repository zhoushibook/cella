#!/usr/bin/env bash
# run_sql_tests.sh -- bash port of cella_sql/tests/run_tests.ps1
#
# Why: the PowerShell version shells out through `cmd /c`, which the host
# Agent blocks. bash can invoke the compiler directly, so we reimplement the
# same checks here (positive golden comparison + negative error-code check).
#
# Usage: ./run_sql_tests.sh [path-to-cella_sql.exe]

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS="$ROOT/cella_sql/tests"

EXE="${1:-}"
if [ -z "$EXE" ]; then
  for c in "$ROOT/build/cella_sql/cella_sql.exe" "$ROOT/build/cella_sql.exe"; do
    [ -f "$c" ] && EXE="$c" && break
  done
fi
if [ -z "$EXE" ] || [ ! -f "$EXE" ]; then
  echo "[失败] 未找到 cella_sql.exe，请先构建。" >&2
  exit 1
fi
echo "使用: $EXE"

# cella_sql.exe is a native Windows binary: it cannot open POSIX "/c/..."
# paths, so convert every path we hand it (and the temp file we redirect to).
winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

pass=0
fail=0

# normalize CRLF -> LF and strip trailing whitespace/newlines, like Normalize()
norm() {
  printf '%s' "$1" | tr -d '\r' | sed -e 's/[[:space:]]*$//' | sed -e :a -e '/^\n*$/{$d;N;};/\n$/ba'
}

# run the compiler, capture stdout+stderr merged.
# Note: bash's $? after a command substitution reflects the last command in
# the pipeline (here: cat), so we must capture the exit code inside without
# letting a pipe clobber it.
run() {
  "$EXE" "$@" >"$TMPOUT" 2>&1
  return $?
}
TMPOUT="$(mktemp)"
trap 'rm -f "$TMPOUT"' EXIT

# ---------- positive cases ----------
for sql in "$TESTS"/sql/ok_*.sql; do
  [ -f "$sql" ] || continue
  name="$(basename "$sql" .sql)"
  expected="$TESTS/expected/${name}_plan.txt"
  if [ ! -f "$expected" ]; then
    echo "[失败] $name : 缺少 golden 文件 expected/${name}_plan.txt"
    fail=$((fail+1)); continue
  fi
  run -a -s -p "$(winpath "$sql")"; code=$?
out="$(cat "$TMPOUT")"
  if [ "$code" -ne 0 ]; then
    echo "[失败] $name : -a -s -p 退出码 $code (期望 0)"
    fail=$((fail+1)); continue
  fi
  run -p "$(winpath "$sql")"
out2="$(cat "$TMPOUT")"
  exp="$(cat "$expected")"
  if [ "$(norm "$out2")" != "$(norm "$exp")" ]; then
    echo "[失败] $name : -p 输出与 golden 不一致"
    fail=$((fail+1)); continue
  fi
  if [[ "$name" == ok_opt_* ]]; then
    expectedOpt="$TESTS/expected/${name}_opt.txt"
    if [ ! -f "$expectedOpt" ]; then
      echo "[失败] $name : 缺少 golden 文件 expected/${name}_opt.txt"
      fail=$((fail+1)); continue
    fi
    run -o "$(winpath "$sql")"
out3="$(cat "$TMPOUT")"
    expOpt="$(cat "$expectedOpt")"
    if [ "$(norm "$out3")" != "$(norm "$expOpt")" ]; then
      echo "[失败] $name : -o 输出与 golden 不一致"
      fail=$((fail+1)); continue
    fi
  fi
  echo "[通过] $name"
  pass=$((pass+1))
done

# ---------- negative cases ----------
for sql in "$TESTS"/sql/err_*.sql; do
  [ -f "$sql" ] || continue
  name="$(basename "$sql" .sql)"
  expect_code="$(head -1 "$sql" | grep -oE 'expect[[:space:]]*:[[:space:]]*[A-Z]+-[0-9]+' | grep -oE '[A-Z]+-[0-9]+' || true)"
  run --all "$(winpath "$sql")"; code=$?
out="$(cat "$TMPOUT")"
  if [ "$code" -ne 1 ]; then
    echo "[失败] $name : --all 退出码 $code (期望 1)"
    fail=$((fail+1)); continue
  fi
  if [ -n "$expect_code" ] && ! printf '%s' "$out" | grep -qF "$expect_code"; then
    echo "[失败] $name : 输出未包含期望错误码 $expect_code"
    fail=$((fail+1)); continue
  fi
  echo "[通过] $name"
  pass=$((pass+1))
done

echo ""
echo "========== 汇总: 通过 $pass / 失败 $fail =========="
[ "$fail" -gt 0 ] && exit 1 || exit 0
