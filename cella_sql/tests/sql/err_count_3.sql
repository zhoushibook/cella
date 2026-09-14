-- expect: SEM-303
-- COUNT(col) 引用的列必须存在
CREATE TABLE t(id INT);
get count(nope) in t;
