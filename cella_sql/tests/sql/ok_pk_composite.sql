-- ok_pk_composite.sql 表级复合主键：PRIMARY KEY (a, b)
CREATE TABLE enroll(sid INT NOT NULL, cid INT NOT NULL, grade FLOAT, PRIMARY KEY (sid, cid));
CREATE TABLE pair(k VARCHAR(8), n INT, PRIMARY KEY (k, n));
insert into enroll(sid,cid,grade) values (1,10,88.5);
insert into enroll values (1,11,90.0),(2,10,75.0);
get sid, cid in enroll ordered sid asc, cid asc;
update enroll set grade = 95.0 limit sid = 1 and cid = 10;
delete in enroll limit sid = 2 and cid = 10;
