-- ok_primary_key.sql 列级主键：PRIMARY KEY 与 NOT NULL 的两种书写顺序
CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);
CREATE TABLE course(code VARCHAR(8) PRIMARY KEY NOT NULL, title TEXT, credit INT NOT NULL);
CREATE TABLE score(sid INT NOT NULL, cid INT NOT NULL PRIMARY KEY, pts INT);
insert into student(id,name) values (1,'Alice');
insert into course values ('C1','Math',3);
update student set name = 'Bob' limit id = 1;
delete in student limit id = 1;
