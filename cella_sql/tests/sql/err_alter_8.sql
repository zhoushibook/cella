-- expect: SEM-325
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
alter table stu add column tag INT PRIMARY KEY;
