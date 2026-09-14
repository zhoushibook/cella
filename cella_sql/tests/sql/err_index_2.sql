-- expect: SEM-303
CREATE TABLE t_a (id INT, n VARCHAR(10));
CREATE INDEX idx_a ON t_a (missing_col);
