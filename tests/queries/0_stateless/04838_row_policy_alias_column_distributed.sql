-- Tags: distributed

-- Row policies and additional_table_filters on ALIAS columns with the old analyzer:
-- on shards (distributed_depth > 0) and with optimize_respect_aliases = 0.

SET enable_analyzer = 0;

DROP ROW POLICY IF EXISTS rp_04838 ON t_04838;
DROP TABLE IF EXISTS t_04838_dist;
DROP TABLE IF EXISTS t_04838;

CREATE TABLE t_04838
(
    k UInt64,
    s String,
    a UInt64 ALIAS k * 10,
    a2 String ALIAS concat(s, toString(a))
)
ENGINE = MergeTree ORDER BY k;

INSERT INTO t_04838 VALUES (1, 'x'), (2, 'y'), (3, 'z'), (4, 'w');

CREATE TABLE t_04838_dist AS t_04838
ENGINE = Distributed('test_shard_localhost', currentDatabase(), 't_04838', rand());

-- Only k = 2 passes: 1 fails `a`, 3 fails `a2` (ALIAS of ALIAS), 4 fails the physical `s`.
CREATE ROW POLICY rp_04838 ON t_04838 FOR SELECT USING s != 'w' AND a > 15 AND a2 != 'z30' TO ALL;

SELECT 'local';
SELECT k FROM t_04838 ORDER BY k;

SELECT 'local, optimize_respect_aliases = 0';
SELECT k FROM t_04838 ORDER BY k SETTINGS optimize_respect_aliases = 0;

SELECT 'local, alias columns selected, optimize_respect_aliases = 0';
SELECT k, a, a2 FROM t_04838 ORDER BY k SETTINGS optimize_respect_aliases = 0;

SELECT 'local, alias columns selected, PREWHERE, optimize_respect_aliases = 0';
SELECT k, a, a2 FROM t_04838 PREWHERE s != 'x' ORDER BY k SETTINGS optimize_respect_aliases = 0;

SELECT 'remote, prefer_localhost_replica = 0';
SELECT k FROM remote('127.0.0.{1,2}', currentDatabase(), t_04838) ORDER BY k SETTINGS prefer_localhost_replica = 0;

SELECT 'remote, prefer_localhost_replica = 1';
SELECT k FROM remote('127.0.0.{1,2}', currentDatabase(), t_04838) ORDER BY k SETTINGS prefer_localhost_replica = 1;

SELECT 'remote, aggregation';
SELECT k, count(), countDistinct(s)
FROM remote('127.0.0.{1,2}', currentDatabase(), t_04838)
WHERE k >= 1
GROUP BY k
ORDER BY k
SETTINGS prefer_localhost_replica = 0;

SELECT 'remote, alias columns selected';
SELECT k, a, a2 FROM remote('127.0.0.{1,2}', currentDatabase(), t_04838) ORDER BY k SETTINGS prefer_localhost_replica = 0;

SELECT 'remote, new analyzer';
SELECT k FROM remote('127.0.0.{1,2}', currentDatabase(), t_04838) ORDER BY k SETTINGS enable_analyzer = 1, prefer_localhost_replica = 0;

SELECT 'distributed, prefer_localhost_replica = 0';
SELECT k FROM t_04838_dist ORDER BY k SETTINGS prefer_localhost_replica = 0;

SELECT 'distributed, prefer_localhost_replica = 1';
SELECT k FROM t_04838_dist ORDER BY k SETTINGS prefer_localhost_replica = 1;

SELECT 'distributed, alias columns selected, PREWHERE, optimize_respect_aliases = 0';
SELECT k, a, a2 FROM t_04838_dist PREWHERE s != 'x' ORDER BY k
SETTINGS prefer_localhost_replica = 0, optimize_respect_aliases = 0;

-- The query itself needs no columns from the table except `k`.
CREATE ROW POLICY OR REPLACE rp_04838 ON t_04838 FOR SELECT USING a2 = 'y20' TO ALL;

SELECT 'distributed, policy with only an ALIAS predicate';
SELECT count(), sum(k) FROM t_04838_dist SETTINGS prefer_localhost_replica = 0;

-- Control: the first policy with ALIAS columns expanded by hand.
CREATE ROW POLICY OR REPLACE rp_04838 ON t_04838 FOR SELECT
USING s != 'w' AND k * 10 > 15 AND concat(s, toString(k * 10)) != 'z30' TO ALL;

SELECT 'distributed, policy with inline expressions';
SELECT k FROM t_04838_dist ORDER BY k SETTINGS prefer_localhost_replica = 0;

DROP ROW POLICY rp_04838 ON t_04838;

-- additional_table_filters are built by the same function (generateFilterActions).
SELECT 'local, additional_table_filters on alias, optimize_respect_aliases = 0';
SELECT k FROM t_04838 ORDER BY k
SETTINGS optimize_respect_aliases = 0, additional_table_filters = {'t_04838': 'a >= 20'};

DROP TABLE t_04838_dist;
DROP TABLE t_04838;
