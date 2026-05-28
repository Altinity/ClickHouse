DROP TABLE IF EXISTS test_marker_collision_dist;
DROP TABLE IF EXISTS test_marker_collision_main;
DROP TABLE IF EXISTS test_marker_collision_left;
DROP TABLE IF EXISTS test_marker_collision_right;

CREATE TABLE test_marker_collision_main
(
    id UInt64
)
ENGINE = MergeTree()
ORDER BY id;

INSERT INTO test_marker_collision_main VALUES (1);

CREATE TABLE test_marker_collision_left
(
    id UInt64,
    x UInt64,
    b UInt64 ALIAS x
)
ENGINE = MergeTree()
ORDER BY id;

CREATE TABLE test_marker_collision_right
(
    id UInt64,
    y UInt64,
    b UInt64 ALIAS y
)
ENGINE = MergeTree()
ORDER BY id;

INSERT INTO test_marker_collision_left VALUES (1, 1);
INSERT INTO test_marker_collision_right VALUES (1, 20);

CREATE TABLE test_marker_collision_dist AS test_marker_collision_main
ENGINE = Distributed('test_shard_localhost', currentDatabase(), test_marker_collision_main, rand());

SELECT 'global_in_collision_check';
SELECT id
FROM test_marker_collision_dist
WHERE id GLOBAL IN
(
    SELECT test_marker_collision_left.id
    FROM test_marker_collision_left
    INNER JOIN test_marker_collision_right
        ON test_marker_collision_left.id = test_marker_collision_right.id
    WHERE test_marker_collision_left.b + test_marker_collision_right.b = 21
)
ORDER BY id
SETTINGS enable_analyzer = 1, enable_alias_marker = 1;

DROP TABLE test_marker_collision_dist;
DROP TABLE test_marker_collision_main;
DROP TABLE test_marker_collision_left;
DROP TABLE test_marker_collision_right;
