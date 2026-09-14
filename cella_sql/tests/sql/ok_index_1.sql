-- ok_index_1.sql 二级索引 DDL：单列普通索引 / 唯一索引 / 删除索引
CREATE TABLE t_user (id INT PRIMARY KEY, name VARCHAR(20), age INT);
CREATE INDEX idx_user_name ON t_user (name);
CREATE UNIQUE INDEX uq_user_age ON t_user (age);
DROP INDEX idx_user_name;
GET * IN t_user;
