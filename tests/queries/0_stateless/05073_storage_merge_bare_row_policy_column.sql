-- A bare row-policy column is the filter input, not a newly computed output.
-- The value must survive filtering when selected, and rows with a false value
-- must be excluded even when the column is used only by the policy.
SET enable_analyzer = 1, enable_alias_marker = 1;

DROP TABLE IF EXISTS t_merge_dist_05073;
DROP TABLE IF EXISTS t_merge_local_05073;
DROP TABLE IF EXISTS t_dist_05073;
DROP TABLE IF EXISTS t_local_05073;

CREATE TABLE t_local_05073
(
    id UInt32,
    visible UInt8,
    flag UInt8 ALIAS if(id = 2, 5, 0)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_local_05073 VALUES (1, 0), (2, 1), (3, 1), (4, 5);

CREATE TABLE t_dist_05073 AS t_local_05073
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), t_local_05073);
CREATE TABLE t_merge_local_05073 (id UInt32, visible UInt8, flag UInt8)
ENGINE = Merge(currentDatabase(), '^t_local_05073$');
CREATE TABLE t_merge_dist_05073 AS t_merge_local_05073
ENGINE = Merge(currentDatabase(), '^t_dist_05073$');

CREATE ROW POLICY rp_05073 ON t_local_05073 USING flag TO ALL;
SELECT 'local alias selected';
SELECT id, flag FROM t_merge_local_05073 ORDER BY id;
SELECT 'local alias policy-only';
SELECT id FROM t_merge_local_05073 ORDER BY id;
DROP ROW POLICY rp_05073 ON t_local_05073;

CREATE ROW POLICY rp_05073 ON t_local_05073 USING visible TO ALL;
SELECT 'local physical selected';
SELECT id, visible FROM t_merge_local_05073 ORDER BY id;
SELECT 'local physical policy-only';
SELECT id FROM t_merge_local_05073 ORDER BY id;
DROP ROW POLICY rp_05073 ON t_local_05073;

CREATE ROW POLICY rp_05073 ON t_dist_05073 USING flag TO ALL;
SELECT 'Distributed AST alias selected';
SELECT id, flag FROM t_merge_dist_05073 GROUP BY id, flag ORDER BY id
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 0;
SELECT 'Distributed plan alias selected';
SELECT id, flag FROM t_merge_dist_05073 GROUP BY id, flag ORDER BY id
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1;
SELECT 'Distributed alias policy-only';
SELECT id FROM t_merge_dist_05073 GROUP BY id ORDER BY id
SETTINGS prefer_localhost_replica = 0;
SELECT 'Distributed plan alias policy-only';
SELECT id FROM t_merge_dist_05073 GROUP BY id ORDER BY id
SETTINGS prefer_localhost_replica = 0, serialize_query_plan = 1;
DROP ROW POLICY rp_05073 ON t_dist_05073;

CREATE ROW POLICY rp_05073 ON t_dist_05073 USING visible TO ALL;
SELECT 'Distributed physical selected';
SELECT id, visible FROM t_merge_dist_05073 GROUP BY id, visible ORDER BY id
SETTINGS prefer_localhost_replica = 0;
DROP ROW POLICY rp_05073 ON t_dist_05073;

DROP TABLE t_merge_dist_05073;
DROP TABLE t_merge_local_05073;
DROP TABLE t_dist_05073;
DROP TABLE t_local_05073;
