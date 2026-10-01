-- Tags: distributed

-- The old interpreter must not use the analyzer-only Distributed path when a view's
-- stored SELECT has SETTINGS enable_analyzer = 1.

DROP VIEW IF EXISTS v_new_04839;
DROP VIEW IF EXISTS v_old_04839;
DROP TABLE IF EXISTS dist_04839;
DROP TABLE IF EXISTS src_04839;

CREATE TABLE src_04839 (trade_date Date, strategy String, pnl Float64, ts DateTime) ENGINE = Memory;
INSERT INTO src_04839 VALUES
    ('2026-09-17', 's1', 1.5, '2026-09-17 10:00:00'),
    ('2026-09-17', 's1', 2.5, '2026-09-17 11:00:00'),
    ('2026-09-17', 's2', 7, '2026-09-17 09:00:00'),
    ('2026-09-16', 's3', 9, '2026-09-16 10:00:00');

CREATE TABLE dist_04839 AS src_04839 ENGINE = Distributed(test_shard_localhost, currentDatabase(), src_04839);

CREATE VIEW v_new_04839 AS
    SELECT strategy, argMax(pnl, ts) AS pnl
    FROM dist_04839 WHERE trade_date = '2026-09-17'
    GROUP BY strategy SETTINGS enable_analyzer = 1;

CREATE VIEW v_old_04839 AS
    SELECT strategy, argMax(pnl, ts) AS pnl
    FROM dist_04839 WHERE trade_date = '2026-09-17'
    GROUP BY strategy SETTINGS enable_analyzer = 0;

SELECT 'view analyzer 1, read analyzer 0';
SELECT count() FROM v_new_04839 SETTINGS enable_analyzer = 0;
SELECT strategy, pnl FROM v_new_04839 ORDER BY strategy SETTINGS enable_analyzer = 0;

SELECT 'view analyzer 1, read analyzer 0, remote shard';
SELECT strategy, pnl FROM v_new_04839 ORDER BY strategy SETTINGS enable_analyzer = 0, prefer_localhost_replica = 0;

SELECT 'view analyzer 1, read analyzer 1';
SELECT strategy, pnl FROM v_new_04839 ORDER BY strategy SETTINGS enable_analyzer = 1;

SELECT 'view analyzer 0, read analyzer 1';
SELECT strategy, pnl FROM v_old_04839 ORDER BY strategy SETTINGS enable_analyzer = 1;

SELECT 'view analyzer 0, read analyzer 0';
SELECT strategy, pnl FROM v_old_04839 ORDER BY strategy SETTINGS enable_analyzer = 0;

DROP VIEW v_new_04839;
DROP VIEW v_old_04839;
DROP TABLE dist_04839;
DROP TABLE src_04839;
