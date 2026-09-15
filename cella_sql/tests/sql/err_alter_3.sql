-- expect: SEM-327
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
alter table stu add primary key (name);
