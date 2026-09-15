-- demo_lang_features.sql —— 视图 / 子查询 / CTE / 窗口函数 执行期演示
-- 运行：cella_db.exe --data <dir> --log off -f cella_db/sql/demo_lang_features.sql
--   或  cella_sql.exe -p -f cella_db/sql/demo_lang_features.sql   （只看计划与结果）
--
-- ⚠ 必须在一个**干净的 --data 目录**上跑：脚本开头是裸的 CREATE TABLE，
--   目录里已有同名表时会报 SEM-302。预期结果是「成功 34 条 / 失败 2 条」，
--   那 2 条失败是脚本末尾**刻意保留**的（视图已 DROP、CTE 不跨语句）。
--
-- 这四类能力此前在编译器层（解析+语义+计划）已经齐备，但执行期一律返回
-- [DB-703] 未实现的特性 —— 因为它们的答案都不在「一行值」的视野里。
-- 本脚本覆盖落地后的常见用法，每条语句都应当给出正确结果。

-- ── 基表 ───────────────────────────────────────────────────────
CREATE TABLE emp(
  id INT PRIMARY KEY,
  name VARCHAR(20),
  dept VARCHAR(10),
  salary INT
);
INSERT INTO emp VALUES (1,'Alice','Eng',12000);
INSERT INTO emp VALUES (2,'Bob','Eng',9000);
INSERT INTO emp VALUES (3,'Carol','Ops',7500);
INSERT INTO emp VALUES (4,'Dave','Ops',6000);

CREATE TABLE dept(
  name VARCHAR(10),
  budget INT
);
INSERT INTO dept VALUES ('Eng',100000);
INSERT INTO dept VALUES ('Ops',40000);


-- ══ 一、子查询 ══════════════════════════════════════════════════
-- 1) IN (子查询)：取「平均薪水以上」的部门里的员工
GET id, name, dept IN emp
LIMIT dept IN (GET dept IN emp LIMIT salary > 8000);

-- 2) NOT IN：与上一条互补
GET id, name IN emp
LIMIT dept NOT IN (GET dept IN emp LIMIT salary > 8000);

-- 3) EXISTS / NOT EXISTS：判定「存在与否」，不取值
GET id, name IN emp LIMIT EXISTS (GET id IN emp LIMIT salary > 11000);
GET id, name IN emp LIMIT NOT EXISTS (GET id IN emp LIMIT salary > 11000);

-- 4) 标量子查询：单个值，可直接参与算术
GET id, name, salary IN emp LIMIT salary > (GET MAX(salary) IN emp) / 2;

-- 5) 标量子查询零行 → NULL（三值逻辑：salary > NULL 是 UNKNOWN，一行都不通过）
GET id IN emp LIMIT salary > (GET MAX(salary) IN emp LIMIT id > 999);

-- 6) 嵌套子查询：内层圈出候选集，外层再取最大
GET id, name IN emp
LIMIT salary = (GET MAX(salary) IN emp LIMIT salary IN (GET salary IN emp LIMIT salary < 10000));


-- ══ 二、窗口函数 ════════════════════════════════════════════════
-- 1) ROW_NUMBER：分区内按薪水降序编号（注意：不重排输出行）
GET id, dept, salary,
    ROW_NUMBER() OVER (PARTITION BY dept ORDERED BY salary DESC) IN emp;

-- 2) RANK（并列跳号）与 DENSE_RANK（并列不跳号）
CREATE TABLE score(id INT, g VARCHAR(4), v INT);
INSERT INTO score VALUES (1,'A',100);
INSERT INTO score VALUES (2,'A',100);
INSERT INTO score VALUES (3,'A',50);
GET id, v,
    RANK() OVER (ORDERED BY v DESC),
    DENSE_RANK() OVER (ORDERED BY v DESC) IN score;

-- 3) 窗口聚合：结果覆盖整个分区（默认帧 = 分区全体）
GET id, dept, salary,
    COUNT(*) OVER (PARTITION BY dept),
    SUM(salary) OVER (PARTITION BY dept),
    AVG(salary) OVER (PARTITION BY dept),
    MIN(salary) OVER (PARTITION BY dept),
    MAX(salary) OVER (PARTITION BY dept) IN emp;

-- 4) 不带 PARTITION BY：整张表当一个分区
GET id, COUNT(*) OVER () IN emp;

-- 5) 窗口结果之上再叠加 ORDERED BY / AMONG
GET id, dept, salary,
    ROW_NUMBER() OVER (PARTITION BY dept ORDERED BY salary DESC) IN emp
ORDERED salary DESC AMONG 3;


-- ══ 三、视图 ════════════════════════════════════════════════════
-- 1) 创建后可直接查询
CREATE VIEW v_eng AS GET id, name, salary IN emp LIMIT dept = 'Eng';
GET * IN v_eng;

-- 2) 视图当普通表用：继续过滤、取列、起别名
GET v.name IN v_eng v LIMIT v.salary > 10000;

-- 3) 视图嵌视图
CREATE VIEW v_top AS GET name, salary IN v_eng LIMIT salary > 10000;
GET * IN v_top;

-- 4) 视图定义持久化在 cella_view 系统表 → 重开引擎后仍然可用（见 --data 目录）
GET name, columns IN cella_view;

-- 5) 删除视图；之后再用它会报「表不存在」
DROP VIEW v_top;
GET * IN v_top;


-- ══ 四、CTE（WITH） ═════════════════════════════════════════════
-- 1) 单个 CTE
WITH rich AS (GET id, name, salary IN emp LIMIT salary > 8000)
GET id, name IN rich;

-- 2) 多个 CTE 串联：后一个可以引用前一个
WITH a AS (GET id, dept, salary IN emp LIMIT salary > 7000),
     b AS (GET id, salary IN a LIMIT dept = 'Eng')
GET * IN b;

-- 3) CTE 只在所属语句内可见 —— 下面这条会报「表 a 不存在」（属预期报错）
WITH a AS (GET id IN emp) GET * IN a;
GET * IN a;
