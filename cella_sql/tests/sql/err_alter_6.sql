-- expect: SEM-328
CREATE TABLE stu (id INT NOT NULL, name VARCHAR(20));
alter table stu drop primary key;
