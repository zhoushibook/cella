-- expect: SEM-303
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
alter table stu drop column nope;
