-- expect: SEM-314
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
alter table stu add column rowid INT;
