-- demo_aggregate.sql —— P4 聚合演示：COUNT(*) / COUNT(col) + 真 GROUP BY
--
-- 覆盖：
--   1. COUNT(*) 全表精确行数（无需索引、无需全表取列再数）
--   2. COUNT(col) 只计非 NULL（SQL 标准语义）
--   3. GROUP BY + COUNT(*) 每组行数
--   4. GROUP BY + COUNT(col) 每组非 NULL 计数
--   5. COUNT(*) 与 COUNT(col) 同处分组
--   6. 真 GROUP BY 语义：非分组列出现在投影里会被拒绝（见文末注释）
--
-- 运行：cella_db.exe --data <dir> -f demo_aggregate.sql

CREATE TABLE sale(id INT, region VARCHAR(16), amount INT, note VARCHAR(16));

INSERT INTO sale VALUES
  (1,'East',100,'ok'),
  (2,'East',NULL,'ok'),
  (3,'West',200,'ok'),
  (4,'West',300,NULL),
  (5,'North',NULL,NULL),
  (6,'North',50,'ok');

-- 1. 全表行数：6
get count(*) in sale;

-- 2. 非 NULL 金额数：4（id=2、5 两行 amount 为 NULL）
get count(amount) in sale;

-- 3. 每组行数：East 2 / North 2 / West 2
get region, count(*) in sale grouped region ordered region asc;

-- 4. 每组非 NULL 金额数：East 1 / North 1 / West 2
get region, count(amount) in sale grouped region ordered region asc;

-- 5. 同一分组里并列多种计数：行数 vs 非 NULL 数
get region, count(*), count(amount), count(note) in sale grouped region ordered region asc;

-- 6. 分组键本身去重（不投影任何非分组列）
get region in sale grouped region ordered region asc;

-- 7. 分组 + 排序 + 取前 N（among）
get region, count(*) in sale grouped region ordered region asc among 2;

-- 8. 全表聚合与过滤组合：只统计 East 的行数
get count(*) in sale limit region = 'East';

-- 9. 空表 COUNT(*) 返回 0（结果集非空）
CREATE TABLE empty_t(id INT);
get count(*) in empty_t;

-- 10. 收尾（保持演示可重复运行）
DROP TABLE sale;
DROP TABLE empty_t;

-- 以下语句会被语义阶段拒绝，演示"真 GROUP BY"的约束：
--   get id, region in sale grouped region;
--   → SEM-322: 列 "id" 既不在 GROUP BY 中也不是聚合函数
--
--   get region, count(*) in sale grouped region having count(*) > 1;
--   → SEM-320: HAVING 中暂不支持聚合函数（P4 范围外）
