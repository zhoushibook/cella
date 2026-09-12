-- ok_rowid.sql rowid 伪列：投影 / 条件 / 排序都是只读用法
CREATE TABLE student(id INT, name VARCHAR(16));
insert into student(id,name) values (1,'Alice');
insert into student(id,name) values (2,'Bob');
get rowid, id, name in student;
get id in student limit rowid = 1;
get id in student ordered rowid asc;
update student set name = 'Ann' limit rowid = 1;
delete in student limit rowid = 1;
