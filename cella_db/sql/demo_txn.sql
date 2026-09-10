-- demo_txn.sql —— 事务与并发控制演示：原子性、隔离（锁）、回滚
-- 覆盖：显式 BEGIN/COMMIT/ROLLBACK、INSERT/DELETE/UPDATE 回滚、
--       语句级原子性（失败语句只撤自己）、DDL 隐式提交前置事务

CREATE TABLE account(id INT NOT NULL, balance INT NOT NULL);

INSERT INTO account VALUES (1,100),(2,50),(3,200);
get * in account ordered id asc;

-- ① 显式事务 + 提交：改动永久生效
BEGIN;
INSERT INTO account VALUES (4,300);
UPDATE account SET balance = 150 limit id = 1;
COMMIT;
get * in account ordered id asc;

-- ② 显式事务 + 回滚：插入被撤销
BEGIN;
INSERT INTO account VALUES (5,999);
ROLLBACK;
get * in account ordered id asc;

-- ③ 显式事务 + 回滚：UPDATE 被撤销（旧值由 undo 日志重插还原）
BEGIN;
UPDATE account SET balance = 0 limit id = 2;
get * in account ordered id asc;
ROLLBACK;
get * in account ordered id asc;

-- ④ 显式事务 + 回滚：DELETE 被撤销
BEGIN;
DELETE in account limit id = 3;
get * in account ordered id asc;
ROLLBACK;
get * in account ordered id asc;

-- ⑤ 语句级原子性：多行插入的第二行违反运行期类型检查，
--    本语句插入的第一行必须一并撤销，但事务继续有效。
BEGIN;
INSERT INTO account VALUES (6,600),(7,1.5);
get * in account ordered id asc;   -- 不应出现 id=6
INSERT INTO account VALUES (6,600);
COMMIT;
get * in account ordered id asc;   -- 出现 id=6

-- ⑥ DDL 隐式提交：BEGIN 之后的建表会先提交前置事务
BEGIN;
INSERT INTO account VALUES (8,800);
CREATE TABLE audit(id INT, note VARCHAR(32));
INSERT INTO audit VALUES (1,'created');
get * in audit;
DROP TABLE audit;
get * in account ordered id asc;   -- id=8 已由隐式提交落定

DROP TABLE account;
