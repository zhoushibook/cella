-- expect: SEM-326
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
alter table stu drop column id;
