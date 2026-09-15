-- expect: SYN-201
CREATE TABLE stu (id INT PRIMARY KEY, name VARCHAR(20));
alter table stu rename name to x;
