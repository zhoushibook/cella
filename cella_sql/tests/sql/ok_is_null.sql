-- ok_is_null.sql IS [NOT] NULL 后缀判空（筛选 NULL 行的唯一合法写法）
CREATE TABLE t(id INT, b VARCHAR(8), n INT);
insert into t(id,b,n) values (1,'x',10);
insert into t(id,b,n) values (2,NULL,NULL);
get * in t limit b is null;
get * in t limit b is not null;
get id in t limit b is null and n is not null;
get id in t limit not b is null;
