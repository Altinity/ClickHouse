DROP TABLE IF EXISTS test_global_alias_chain_dist;
DROP TABLE IF EXISTS test_global_alias_chain_local;

CREATE TABLE test_global_alias_chain_local
(
    id UInt64,
    base UInt64,
    a UInt64 ALIAS base,
    b UInt64 ALIAS a
)
ENGINE = MergeTree()
ORDER BY id;

INSERT INTO test_global_alias_chain_local VALUES (1, 1);

CREATE TABLE test_global_alias_chain_dist AS test_global_alias_chain_local
ENGINE = Distributed('test_cluster_two_shards', currentDatabase(), test_global_alias_chain_local, rand());

SELECT 'rewrite_in';
SELECT id
FROM test_global_alias_chain_dist
WHERE id IN (SELECT b FROM test_global_alias_chain_dist)
ORDER BY id
SETTINGS enable_analyzer = 1, distributed_product_mode = 'global';

SELECT 'rewrite_join';
SELECT l.id
FROM test_global_alias_chain_dist AS l
INNER JOIN (SELECT b FROM test_global_alias_chain_dist) AS r ON l.id = r.b
ORDER BY l.id
SETTINGS enable_analyzer = 1, distributed_product_mode = 'global';

DROP TABLE test_global_alias_chain_dist;
DROP TABLE test_global_alias_chain_local;
