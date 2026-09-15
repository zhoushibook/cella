-- expect: SEM-323
CREATE TABLE solo (a INT);
alter table solo drop column a;
