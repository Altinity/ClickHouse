-- A scalar subquery is evaluated once and sent to every shard as `__getScalar`.
-- It must not stay a GROUP BY key just because that function is marked as a server constant.
-- https://github.com/Altinity/ClickHouse/pull/2490

SELECT count() FROM numbers(3) GROUP BY (SELECT [1, 2]) SETTINGS enable_analyzer = 0;

SELECT count() FROM numbers(3) GROUP BY (SELECT [CAST(1, 'Dynamic')]) SETTINGS enable_analyzer = 0;
