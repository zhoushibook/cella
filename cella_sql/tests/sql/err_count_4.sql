-- expect: SYN-201
-- COUNT 必须有括号参数：COUNT 单独出现（如 COUNT in t）是语法错误
CREATE TABLE t(id INT);
get count in t;
