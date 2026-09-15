#!/bin/bash
run_q() {
  local tag="$1" sql="$2"
  printf 'EXPLAIN %s\n' "$sql" > testdata/_q.sql
  local path_line
  path_line=$(./build/cella_db/cella_db.exe --data testdata/db_fixed --pool 3000 -f testdata/_q.sql 2>/dev/null \
    | grep -E "IndexOnlyScan|IndexScan|SeqScan \[" | tail -1 | sed 's/ *$//')
  printf '%s\n' "$sql" > testdata/_q.sql
  local out timing rows acc
  out=$(./build/cella_db/cella_db.exe --data testdata/db_fixed --pool 3000 -f testdata/_q.sql --timing --stats 2>/dev/null)
  timing=$(echo "$out" | grep -oE "Time: [0-9.]+ ms" | head -1 | grep -oE "[0-9.]+")
  rows=$(echo "$out" | grep -oE "\([0-9]+ 行\)" | head -1 | grep -oE "[0-9]+")
  acc=$(echo "$out" | grep -oE "accesses *: [0-9]+" | grep -oE "[0-9]+$")
  miss=$(echo "$out" | grep -oE "misses *: [0-9]+" | grep -oE "[0-9]+$")
  printf "%s | %s ms | %s 行 | 访问 %s (缺页 %s) | %s\n" "$tag" "$timing" "$rows" "$acc" "$miss" "$path_line"
}
run_q "Q1_主键点查"        "get name, age in users limit id = 12345;"
run_q "Q2_二级索引等值(高选择性)" "get id in orders limit user_id = 777;"
run_q "Q3_二级索引范围"     "get id in orders limit user_id >= 100 and user_id <= 199;"
run_q "Q4_二级索引等值(25%选择率)" "get id in orders limit status = 'paid' among 20;"
run_q "Q5_无索引列等值"     "get tag, val in events limit val = 500;"
run_q "Q6_无索引表排序"     "get tag, val in events ordered val desc among 3;"
run_q "Q7_主键范围(连续页)"  "get id, name in users limit id >= 20000 and id <= 20009;"
run_q "Q8_有索引表全量排序"  "get id, name in users ordered id desc among 3;"
