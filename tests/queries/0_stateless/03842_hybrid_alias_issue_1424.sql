SET allow_experimental_hybrid_table = 1, enable_analyzer = 1;

DROP TABLE IF EXISTS test_hybrid_issue_1424;
DROP TABLE IF EXISTS test_hybrid_issue_1424_left;
DROP TABLE IF EXISTS test_hybrid_issue_1424_right;
DROP TABLE IF EXISTS test_hybrid_issue_1424_const;
DROP TABLE IF EXISTS test_hybrid_issue_1424_const_left;
DROP TABLE IF EXISTS test_hybrid_issue_1424_const_right;

CREATE TABLE test_hybrid_issue_1424_left
(
    id Int32,
    value Int32,
    date_col Date,
    computed ALIAS value * 2
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(date_col)
ORDER BY (date_col, id);

INSERT INTO test_hybrid_issue_1424_left VALUES
    (toInt32(2147483647), toInt32(2147483647), toDate('2149-06-06')),
    (toInt32(-2147483648), toInt32(-2147483648), toDate('1970-01-01')),
    (toInt32(0), toInt32(0), '1970-01-01'),
    (toInt32(1084637461), toInt32(1708739853), toDate(1335613783)),
    (toInt32(-221724287), toInt32(339886211), toDate(1294089763)),
    (toInt32(-1762862292), toInt32(-287306889), toDate(1375707465)),
    (toInt32(1169291374), toInt32(-1541024731), toDate(1082126480)),
    (toInt32(-1329695183), toInt32(-786819168), toDate(1226000164)),
    (toInt32(1899628504), toInt32(-370080625), toDate(1179050966)),
    (toInt32(550067609), toInt32(-1524000367), toDate(1410654931));

CREATE TABLE test_hybrid_issue_1424_right
(
    id Int32,
    value Int32,
    date_col Date,
    computed ALIAS value * 2
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(date_col)
ORDER BY (date_col, id);

INSERT INTO test_hybrid_issue_1424_right VALUES
    (toInt32(2147483647), toInt32(2147483647), toDate('2149-06-06')),
    (toInt32(-2147483648), toInt32(-2147483648), toDate('1970-01-01')),
    (toInt32(0), toInt32(0), '1970-01-01'),
    (toInt32(1084637461), toInt32(1708739853), toDate(1335613783)),
    (toInt32(-221724287), toInt32(339886211), toDate(1294089763)),
    (toInt32(-1762862292), toInt32(-287306889), toDate(1375707465)),
    (toInt32(1169291374), toInt32(-1541024731), toDate(1082126480)),
    (toInt32(-1329695183), toInt32(-786819168), toDate(1226000164)),
    (toInt32(1899628504), toInt32(-370080625), toDate(1179050966)),
    (toInt32(550067609), toInt32(-1524000367), toDate(1410654931));

CREATE TABLE test_hybrid_issue_1424
(
    id Int32,
    value Int32,
    date_col Date,
    computed Int64
)
ENGINE = Hybrid(
    remote('127.0.0.1:9000', currentDatabase(), 'test_hybrid_issue_1424_left'), date_col >= '2025-01-15',
    remote('127.0.0.1:9000', currentDatabase(), 'test_hybrid_issue_1424_right'), date_col < '2025-01-15'
);

SELECT 'max in subquery';
SELECT max_computed FROM (SELECT max(computed) AS max_computed FROM test_hybrid_issue_1424);

SELECT 'sum in subquery';
SELECT sum_computed FROM (SELECT sum(computed) AS sum_computed FROM test_hybrid_issue_1424);

SELECT 'cte min with predicate';
WITH cte AS
(
    SELECT min(computed) AS min_computed
    FROM test_hybrid_issue_1424
    WHERE computed > 50
)
SELECT * FROM cte;

SELECT 'cte with limit';
WITH ranked AS
(
    SELECT id, computed
    FROM test_hybrid_issue_1424
    LIMIT 10
)
SELECT *
FROM ranked
ORDER BY id ASC;

SELECT 'cte without limit';
WITH ranked AS
(
    SELECT id, computed
    FROM test_hybrid_issue_1424
)
SELECT *
FROM ranked
ORDER BY id ASC;

SELECT 'group by in subquery';
WITH monthly AS
(
    SELECT count() AS cnt
    FROM test_hybrid_issue_1424
    GROUP BY computed
)
SELECT sum(cnt), count() FROM monthly;

SELECT 'intersect with order by';
SELECT *
FROM
(
    SELECT id, computed
    FROM test_hybrid_issue_1424
    WHERE computed > 100
    INTERSECT
    SELECT id, computed
    FROM test_hybrid_issue_1424
    WHERE value > 50
)
ORDER BY id;

SELECT 'intersect without order by';
SELECT *
FROM
(
    SELECT id, computed
    FROM test_hybrid_issue_1424
    WHERE computed > 100
    INTERSECT
    SELECT id, computed
    FROM test_hybrid_issue_1424
    WHERE value > 50
)
ORDER BY id;

CREATE TABLE test_hybrid_issue_1424_const_left
(
    id Int32,
    value Int32,
    date_col Date,
    computed ALIAS toInt64(7)
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(date_col)
ORDER BY (date_col, id);

INSERT INTO test_hybrid_issue_1424_const_left VALUES
    (1, 1, toDate('2025-01-15')),
    (2, 2, toDate('2025-02-01'));

CREATE TABLE test_hybrid_issue_1424_const_right
(
    id Int32,
    value Int32,
    date_col Date,
    computed ALIAS toInt64(9)
)
ENGINE = MergeTree
PARTITION BY toYYYYMM(date_col)
ORDER BY (date_col, id);

INSERT INTO test_hybrid_issue_1424_const_right VALUES
    (3, 3, toDate('2024-12-31')),
    (4, 4, toDate('2020-01-01'));

CREATE TABLE test_hybrid_issue_1424_const
(
    id Int32,
    value Int32,
    date_col Date,
    computed Int64
)
ENGINE = Hybrid(
    remote('127.0.0.1:9000', currentDatabase(), 'test_hybrid_issue_1424_const_left'), date_col >= '2025-01-15',
    remote('127.0.0.1:9000', currentDatabase(), 'test_hybrid_issue_1424_const_right'), date_col < '2025-01-15'
);

SELECT 'constant alias in subquery';
SELECT max_computed, min_computed, sum_computed
FROM
(
    SELECT
        max(computed) AS max_computed,
        min(computed) AS min_computed,
        sum(computed) AS sum_computed
    FROM test_hybrid_issue_1424_const
);

SELECT 'constant alias predicate';
SELECT count() FROM test_hybrid_issue_1424_const WHERE computed = 9;

DROP TABLE test_hybrid_issue_1424;
DROP TABLE test_hybrid_issue_1424_left;
DROP TABLE test_hybrid_issue_1424_right;
DROP TABLE test_hybrid_issue_1424_const;
DROP TABLE test_hybrid_issue_1424_const_left;
DROP TABLE test_hybrid_issue_1424_const_right;
