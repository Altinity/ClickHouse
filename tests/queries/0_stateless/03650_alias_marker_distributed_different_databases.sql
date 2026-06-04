CREATE DATABASE IF NOT EXISTS shard_0;
CREATE DATABASE IF NOT EXISTS shard_1;

DROP TABLE IF EXISTS alias_marker_dist;
DROP TABLE IF EXISTS shard_0.alias_marker_src;
DROP TABLE IF EXISTS shard_1.alias_marker_src;

CREATE TABLE shard_0.alias_marker_src
(
    number UInt64,
    foo Int64
)
ENGINE = MergeTree()
ORDER BY number;

CREATE TABLE shard_1.alias_marker_src
(
    number UInt64,
    foo ALIAS number * 2 - 3
)
ENGINE = MergeTree()
ORDER BY number;

INSERT INTO shard_0.alias_marker_src (number, foo) VALUES (1, -1), (2, 1), (3, 3);
INSERT INTO shard_1.alias_marker_src (number) VALUES (4), (5), (6);

CREATE TABLE alias_marker_dist
(
    number UInt64,
    foo Int64
)
ENGINE = Distributed('test_cluster_two_shards_different_databases', '', 'alias_marker_src', rand());

SELECT foo, sum(number)
FROM alias_marker_dist
GROUP BY foo
ORDER BY foo
SETTINGS enable_alias_marker=1, prefer_localhost_replica=1
FORMAT TabSeparatedWithNames;

DROP TABLE alias_marker_dist;
DROP TABLE shard_0.alias_marker_src;
DROP TABLE shard_1.alias_marker_src;
