-- expect: SEM-302
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
CREATE TABLE other (x INT);
alter table stu rename to other;
