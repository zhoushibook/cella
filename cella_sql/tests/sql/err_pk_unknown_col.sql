-- expect: SEM-303
CREATE TABLE t(a INT, b INT, PRIMARY KEY (a, missing));
