-- expect: SEM-315
CREATE TABLE t_b (id INT, n VARCHAR(10));
CREATE INDEX idx_b ON t_b (n);
CREATE INDEX IDX_B ON t_b (id);
