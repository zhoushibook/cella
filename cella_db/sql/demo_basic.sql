-- demo_basic.sql —— 基础链路演示：DDL → INSERT → GET → UPDATE → DELETE
-- 覆盖：NOT NULL 约束、列清单/全列插入、多行插入、WHERE(limit) 过滤、
--       ORDER BY(ordered)、LIMIT(among)、类型不匹配报错、语句级原子性

CREATE TABLE student(
  id    INT NOT NULL,
  name  VARCHAR(32) NOT NULL,
  age   INT,
  score DOUBLE,
  class VARCHAR(8)
);

-- 逐行插入
INSERT INTO student(id,name,age,score,class) VALUES (1,'Alice',20,88.5,'A');
-- 多行插入（一次语句写多行）
INSERT INTO student(id,name,age,score,class) VALUES
  (2,'Bob',19,76.0,'A'),
  (3,'Carol',22,94.5,'B'),
  (4,'Dave',21,61.0,'B'),
  (5,'Eve',20,80.0,'C');

-- 全列查询
get * in student;

-- 投影 + 过滤 + 排序
get id, name, age in student limit age >= 20 and score > 80 ordered name asc;

-- 排序 + 取前 N
get name, score in student ordered score desc among 3;

-- 更新（存储层无原地更新，由数据库层实现为删旧+插新，旧值进回滚日志）
UPDATE student SET score = 85.0, class = 'B' limit id = 4;
get id, name, score, class in student limit id = 4;

-- 删除
DELETE in student limit name = 'Bob';
get id, name in student ordered id asc;

-- ① 约束：向 NOT NULL 列插入 NULL（编译期即被语义阶段拦下）
INSERT INTO student(id,name,age) VALUES (9,NULL,20);

-- ② 约束：字符串写入数值列（编译期类型不匹配）
INSERT INTO student(id,name,age) VALUES (9,'Zoe','twenty');

-- ③ 运行期错误：小数写入 INT 列 —— 语义阶段允许 NUMBER→INT，
--    执行阶段拒绝。多行插入的第一行已写入，语句级回滚应把它撤掉。
INSERT INTO student(id,name,age) VALUES (10,'Frank',20),(11,'Grace',20.5);
get id, name in student ordered id asc;

DROP TABLE student;
