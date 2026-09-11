DROP TABLE IF EXISTS test_nested_alias_dist;
DROP TABLE IF EXISTS test_nested_alias_local;

CREATE TABLE test_nested_alias_local
(
    dt DateTime64(3),
    base String,
    a String ALIAS base,
    b String ALIAS a
)
ENGINE = MergeTree()
ORDER BY dt;

INSERT INTO test_nested_alias_local VALUES ('1999-03-29T01:15:33', 'x');

CREATE TABLE test_nested_alias_dist AS test_nested_alias_local
ENGINE = Distributed('test_shard_localhost', currentDatabase(), test_nested_alias_local, rand());

SELECT 'analyzer';
SELECT a, b
FROM test_nested_alias_dist
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 1;

SELECT 'legacy';
SELECT a, b
FROM test_nested_alias_dist
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 0;

DROP TABLE test_nested_alias_dist;
DROP TABLE test_nested_alias_local;
