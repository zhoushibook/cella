-- ok_alter_1.sql ALTER TABLE 六种动作 + TRUNCATE TABLE（P5 DDL 演进）
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL);
insert into stu values (1, 'amy'), (2, 'bob');
alter table stu add column age INT;
alter table stu drop column age;
alter table stu rename column name to sname;
alter table stu rename to student;
alter table student drop primary key;
alter table student add primary key (id);
create index idx_stu_sname on student(sname);
alter table student drop column sname;
truncate table student;
CREATE TABLE empty_t (k INT);
alter table empty_t add column note VARCHAR(10) NOT NULL;
insert into empty_t values (1, 'hi');
truncate table empty_t;
