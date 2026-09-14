-- expect: SEM-322
-- 出现 GROUP BY 时，SELECT 项只能是分组键或聚合函数
CREATE TABLE t(id INT, grp VARCHAR(8));
get id in t grouped grp;
