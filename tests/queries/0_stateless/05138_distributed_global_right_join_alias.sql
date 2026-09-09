-- Tags: distributed

SET enable_analyzer = 1;
SET join_use_nulls = 1;

DROP TABLE IF EXISTS left_local_05138;
DROP TABLE IF EXISTS right_local_05138;
DROP TABLE IF EXISTS left_distributed_05138;
DROP TABLE IF EXISTS right_distributed_05138;
DROP TABLE IF EXISTS left_alias_05138;

CREATE TABLE left_local_05138 (k1 UInt32, v1 String)
ENGINE = MergeTree
ORDER BY k1;

CREATE TABLE right_local_05138 (k2 UInt32, v2 String)
ENGINE = MergeTree
ORDER BY k2;

CREATE TABLE left_distributed_05138 AS left_local_05138
ENGINE = Distributed('test_cluster_two_shards_localhost', currentDatabase(), left_local_05138);

CREATE TABLE right_distributed_05138 AS right_local_05138
ENGINE = Distributed('test_cluster_two_shards_localhost', currentDatabase(), right_local_05138);

CREATE TABLE left_alias_05138
ENGINE = Alias('left_distributed_05138');

INSERT INTO left_local_05138 VALUES (1, 'a'), (2, 'b'), (4, 'd');
INSERT INTO right_local_05138 VALUES (1, 'A'), (2, 'B'), (3, 'C');

SELECT 'initiator';
SELECT *
FROM (SELECT * FROM left_alias_05138) AS l
RIGHT JOIN (SELECT * FROM right_distributed_05138) AS r ON l.k1 = r.k2
ORDER BY ALL
FORMAT TSVWithNames;

-- An `Alias` of a sharded `Distributed` table still fans the query out across shards,
-- so the rewrite must look through it the same way as a bare `Distributed` table.
SELECT 'alias_global';
SELECT *
FROM left_alias_05138 AS l
GLOBAL RIGHT JOIN right_distributed_05138 AS r ON l.k1 = r.k2
ORDER BY ALL
FORMAT TSVWithNames;

DROP TABLE left_alias_05138;
DROP TABLE left_distributed_05138;
DROP TABLE right_distributed_05138;
DROP TABLE left_local_05138;
DROP TABLE right_local_05138;
