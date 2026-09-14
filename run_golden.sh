#!/usr/bin/env bash
# 编译器 golden 契约校验：正向逐字节比对 -p 输出，负向检查错误码。
cd "$(dirname "$0")"
EXE=build/cella_sql/cella_sql.exe
pass=0; fail=0
for f in cella_sql/tests/sql/ok_*.sql; do
  n=$(basename "$f" .sql)
  exp="cella_sql/tests/expected/${n}_plan.txt"
  "$EXE" -p "$f" > /tmp/g_act.txt 2>&1
  sed -i 's/\r$//' /tmp/g_act.txt
  sed 's/\r$//' "$exp" > /tmp/g_exp.txt
  # 忽略末尾空行差异（空 golden 的产物）
  if [ "$(sed -e :a -e '/^$/{$d;N;ba' -e '}' /tmp/g_exp.txt)" = "$(sed -e :a -e '/^$/{$d;N;ba' -e '}' /tmp/g_act.txt)" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "[GOLDEN DIFF] $n"
  fi
done
for f in cella_sql/tests/sql/err_*.sql; do
  n=$(basename "$f" .sql)
  want=$(head -1 "$f" | sed -n 's/.*expect: *\([A-Za-z-]*[0-9]*\).*/\1/p')
  out=$("$EXE" -p "$f" 2>&1)
  if [ -n "$want" ] && echo "$out" | grep -q "$want"; then pass=$((pass+1)); else fail=$((fail+1)); echo "[NEG FAIL] $n want=$want"; fi
done
echo "========== golden 通过 $pass / 失败 $fail =========="
[ "$fail" -eq 0 ]
