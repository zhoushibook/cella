-- demo_functions.sql —— 标量函数 / BOOLEAN / LIKE / IN 列表 / 约束语法 演示
-- 运行：cella_db.exe --data <dir> --log off -f cella_db/sql/demo_functions.sql
--
-- 覆盖的能力：
--   * 字符串函数 UPPER / LOWER / LENGTH / SUBSTR / CONCAT / REPLACE / TRIM(LTRIM/RTRIM)
--   * 数值函数   ABS / ROUND / CEIL / FLOOR
--   * 日期函数   YEAR / MONTH / DAY / DATEDIFF / DATE_ADD / DATE_SUB
--   * BOOLEAN 列（TRUE / FALSE 字面量，存储层 kBool）
--   * LIKE / NOT LIKE 通配比较
--   * IN ( 常量列表 ) / NOT IN
--   * DDL 约束语法 DEFAULT / CHECK / UNIQUE / REFERENCES（编译期校验，执行期强制见文档）

CREATE TABLE emp(
  id INT PRIMARY KEY,
  name VARCHAR(20) NOT NULL,
  dept VARCHAR(10),
  salary INT DEFAULT 5000 CHECK (salary > 0),
  joined DATE,
  active BOOLEAN DEFAULT TRUE
);

INSERT INTO emp VALUES (1,'Alice','Eng',12000,'2024-03-15',TRUE);
INSERT INTO emp VALUES (2,'Bob','Eng',9000,'2025-07-01',FALSE);
INSERT INTO emp VALUES (3,'Carol','Ops',7500,'2023-11-20',TRUE);

-- 字符串函数
GET id, UPPER(name), LOWER(dept), LENGTH(name) IN emp;
GET id, SUBSTR(name, 1, 3), CONCAT(dept, '-', name) IN emp;
GET id, TRIM(name), REPLACE(dept, 'Eng', 'RD') IN emp;

-- 日期函数（日期以 YYYY-MM-DD 文本存储，运算时换算为天数）
GET id, YEAR(joined), MONTH(joined), DAY(joined) IN emp;
GET id, DATEDIFF('2026-01-01', joined) IN emp;
GET id, DATE_ADD(joined, 30), DATE_SUB(joined, 10) IN emp;

-- 数值函数
GET id, ABS(0 - salary), ROUND(salary / 7.0, 2), CEIL(salary / 7.0),
    FLOOR(salary / 7.0) IN emp;

-- BOOLEAN 列
GET id, name, active IN emp LIMIT active = TRUE;
GET id, name IN emp LIMIT active = FALSE;

-- LIKE / NOT LIKE（'%' 任意长度，'_' 单个字符，区分大小写）
GET id, name IN emp LIMIT name LIKE 'A%';
GET id, name IN emp LIMIT name NOT LIKE 'A%';
GET id, name IN emp LIMIT name LIKE 'Ca_ol';

-- IN / NOT IN 常量列表
GET id, name IN emp LIMIT dept IN ('Eng', 'Ops');
GET id, name IN emp LIMIT dept NOT IN ('Ops');
GET id, name IN emp LIMIT id IN (1, 3);

-- 与其它子句组合
GET id, salary IN emp LIMIT dept IN ('Eng', 'Ops') ORDERED salary DESC;
