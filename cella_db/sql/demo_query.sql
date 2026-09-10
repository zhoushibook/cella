-- demo_query.sql —— 查询能力演示：连接 / 去重 / 分组 / 并集 / 分页 / 计划优化
-- 覆盖：middle/left/right join、distinct、grouped、having、union、
--       among、page、表达式计算、常量折叠与恒真谓词消除

CREATE TABLE course(id INT, title VARCHAR(32));
CREATE TABLE student(id INT, name VARCHAR(32), cid INT);

INSERT INTO course VALUES (1,'Math'),(2,'Physics'),(3,'Chemistry');
INSERT INTO student VALUES (1,'Alice',1),(2,'Bob',3),(3,'Carol',2),(4,'Dave',1);

-- 内连接（middle join）
get s.name, c.title in student s middle join course c on s.cid = c.id ordered s.name asc;

-- 左外连接：无匹配课程的学生补 NULL
INSERT INTO student VALUES (5,'Frank',99);
get s.name, c.title in student s left join course c on s.cid = c.id ordered s.name asc;

-- 右外连接：无学生的课程补 NULL
get s.name, c.title in student s right join course c on s.cid = c.id ordered c.title asc;

-- 去重
get distinct cid in student ordered cid asc;

-- 分组（本方言无聚合函数，GROUP BY 即按分组键去重）+ having
get cid in student grouped cid having cid = 1;

-- 并集
get name in student limit cid = 1 union get name in student limit cid = 2;

-- 排序 + 分页（page 页码, 每页行数；页码从 1 起）
get id, name in student ordered id asc page 1, 2;
get id, name in student ordered id asc page 2, 2;

-- among 先限总行数，再分页
get id, name in student ordered id asc among 3 page 2, 2;

-- 表达式投影：算术与常量折叠（10 + 8 在编译期折叠为 18）
get id, name, cid + 1 as next_cid in student ordered id asc;
get name in student limit 1 = 1 and cid > 10 + 8;
get name in student limit 1 = 1;

DROP TABLE student;
DROP TABLE course;
