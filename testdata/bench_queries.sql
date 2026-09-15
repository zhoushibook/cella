-- 性能基准查询组（在 testdata/db_fixed 上运行，10 万行）
-- 运行方式：每条单独一个进程 + --timing --stats 看耗时与缓冲池访问量
-- 或用 testdata/_bench.sh 逐条冷进程实测并汇总

-- Q1 主键点查（最快路径：1 次树下降 + 1 次回表）
get name, age in users limit id = 12345;

-- Q2 二级索引等值，高选择性（5 万行里命中 ~2 行）
get id in orders limit user_id = 777;

-- Q3 二级索引范围（AND 两侧下推，取交集）
get id in orders limit user_id >= 100 and user_id <= 199;

-- Q4 二级索引等值，低选择性（25% ≈ 1.25 万行）+ among 截断
-- 注意：实测优化器把 rows 估成 1，仍选了索引 —— 但索引扫了全部 1.25 万个键
get id in orders limit status = 'paid' among 20;

-- Q5 无索引列等值（events 无任何索引，纯全表扫对照）
get tag, val in events limit val = 500;

-- Q6 无索引表排序（15k 行物化 + 排序）
get tag, val in events ordered val desc among 3;

-- Q7 主键范围（id 连续 → 回表落在相邻页，几乎顺序 I/O）
get id, name in users limit id >= 20000 and id <= 20009;

-- Q8 有索引表按主键倒序取前 3（优化器没拿索引当排序用 → SeqScan + 全量排序）
get id, name in users ordered id desc among 3;

-- 对照：EXPLAIN 看每条的真实访问路径（例）
-- EXPLAIN get id in orders limit user_id = 777;
