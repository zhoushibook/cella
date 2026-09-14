-- expect: SEM-320
-- HAVING 中暂不支持聚合函数（本阶段范围外）
CREATE TABLE t(id INT, grp VARCHAR(8));
get grp, count(*) in t grouped grp having count(*) > 1;
