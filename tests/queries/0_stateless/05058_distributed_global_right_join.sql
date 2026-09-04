-- Tags: distributed

SET enable_analyzer = 1;
SET join_use_nulls = 1;

DROP TABLE IF EXISTS left_local_05058;
DROP TABLE IF EXISTS right_local_05058;
DROP TABLE IF EXISTS left_distributed_05058;
DROP TABLE IF EXISTS right_distributed_05058;

CREATE TABLE left_local_05058 (k1 UInt32, v1 String)
ENGINE = MergeTree
ORDER BY k1;

CREATE TABLE right_local_05058 (k2 UInt32, v2 String)
ENGINE = MergeTree
ORDER BY k2;

CREATE TABLE left_distributed_05058 AS left_local_05058
ENGINE = Distributed('test_cluster_two_shards_localhost', currentDatabase(), left_local_05058);

CREATE TABLE right_distributed_05058 AS right_local_05058
ENGINE = Distributed('test_cluster_two_shards_localhost', currentDatabase(), right_local_05058);

INSERT INTO left_local_05058 VALUES (1, 'a'), (2, 'b'), (4, 'd');
INSERT INTO right_local_05058 VALUES (1, 'A'), (2, 'B'), (3, 'C');

-- The subqueries force the join to run on the initiator and define the correct result.
SELECT 'initiator';
SELECT *
FROM (SELECT * FROM left_distributed_05058) AS l
RIGHT JOIN (SELECT * FROM right_distributed_05058) AS r ON l.k1 = r.k2
ORDER BY ALL
FORMAT TSVWithNames;

-- The global join must produce the same rows and preserve the original column order.
SELECT 'distributed_product_mode';
SELECT *
FROM left_distributed_05058 AS l
RIGHT JOIN right_distributed_05058 AS r ON l.k1 = r.k2
ORDER BY ALL
SETTINGS distributed_product_mode = 'global'
FORMAT TSVWithNames;

SELECT 'explicit_global';
SELECT *
FROM left_distributed_05058 AS l
GLOBAL RIGHT JOIN right_distributed_05058 AS r ON l.k1 = r.k2
ORDER BY ALL
FORMAT TSVWithNames;

DROP TABLE left_distributed_05058;
DROP TABLE right_distributed_05058;
DROP TABLE left_local_05058;
DROP TABLE right_local_05058;
