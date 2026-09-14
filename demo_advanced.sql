-- ============ cella 高级能力现场演示 ============
-- 1) 列级主键（隐含 NOT NULL）+ rowid 伪列
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20), age INT);
INSERT INTO stu VALUES (1, 'alice', 20);
INSERT INTO stu VALUES (2, 'bob', 22);
INSERT INTO stu VALUES (3, 'carol', 21);

-- rowid 伪列：每张表都有的只读定位列
GET rowid, id, name IN stu;

-- 2) 二级索引 DDL（本次新增）
CREATE INDEX idx_stu_name ON stu (name);
CREATE UNIQUE INDEX uq_stu_age ON stu (age);
SHOW INDEXES;
SHOW INDEXES IN stu;

-- 3) 三向连接（内 / 左 / 右）
CREATE TABLE dept (did INT PRIMARY KEY, dname VARCHAR(20));
INSERT INTO dept VALUES (10, 'eng');
INSERT INTO dept VALUES (20, 'ops');
CREATE TABLE emp (eid INT PRIMARY KEY, ename VARCHAR(20), dept_id INT);
INSERT INTO emp VALUES (100, 'alice', 10);
INSERT INTO emp VALUES (101, 'bob', 20);
INSERT INTO emp VALUES (102, 'zed', 99);
GET emp.ename, dept.dname IN emp JOIN dept ON emp.dept_id = dept.did;
GET emp.ename, dept.dname IN emp LEFT JOIN dept ON emp.dept_id = dept.did;
GET emp.ename, dept.dname IN emp RIGHT JOIN dept ON emp.dept_id = dept.did;

-- 4) 分组 + HAVING + 排序 + 去重
GET dept_id IN emp GROUPED dept_id HAVING dept_id > 5 ORDERED dept_id ASC;
GET DISTINCT dept_id IN emp;

-- 5) UNION
GET id IN stu UNION GET did IN dept;

-- 6) LIMIT / 原生分页
GET * IN stu AMONG 2;
GET * IN stu PAGE 1 2;

-- 7) IS [NOT] NULL
INSERT INTO stu VALUES (4, NULL, 30);
GET name IN stu limit name IS NULL;
GET name IN stu limit name IS NOT NULL;

-- 8) 表达式与布尔化简
GET id, age + 1 IN stu limit age > 18 AND NOT (age < 0);

-- 9) DROP INDEX / DROP TABLE 级联
DROP INDEX idx_stu_name;
SHOW INDEXES;
DROP TABLE dept;
SHOW INDEXES;
