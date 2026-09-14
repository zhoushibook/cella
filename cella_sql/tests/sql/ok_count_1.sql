-- ok_count_1.sql 聚合：COUNT(*) / COUNT(col) + grouped（P4）
CREATE TABLE sale(id INT, region VARCHAR, amount INT);
INSERT INTO sale VALUES (1,'East',100),(2,'East',NULL),(3,'West',200),(4,'West',300),(5,'North',NULL);
get count(*) in sale;
get count(amount) in sale;
get region, count(*) in sale grouped region ordered region asc;
get region, count(amount) in sale grouped region ordered region asc;
get region, count(*), count(amount) in sale grouped region ordered region asc among 2;
