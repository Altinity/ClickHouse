-- `enable_alias_marker` must not change results. It exists so an initiator can stop emitting
-- `__aliasMarker` for a mixed-version cluster whose shards do not understand it, which is a
-- transport concern, not a correctness one.
--
-- Distributed-over-distributed with a String ALIAS (`a_str`) and a UInt64 ALIAS (`inner_c`). This
-- shape used to swap the two columns with the marker off, routing the String 'aaaa' into the
-- UInt64 `inner_c` slot and failing with CANNOT_PARSE_TEXT. Upstream fixed the underlying column
-- ordering, so both settings now return the same rows, and this test holds them to that.
DROP TABLE IF EXISTS t_se_local;
DROP TABLE IF EXISTS t_se_inner;
DROP TABLE IF EXISTS t_se_outer;

CREATE TABLE t_se_local (x UInt64) ENGINE = MergeTree() ORDER BY x;
INSERT INTO t_se_local VALUES (1), (2), (10);

CREATE TABLE t_se_inner (x UInt64, inner_c UInt64 ALIAS x + 1)
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), t_se_local);

CREATE TABLE t_se_outer (x UInt64, inner_c UInt64, a_str String ALIAS 'aaaa')
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), t_se_inner);

-- serialize_query_plan is pinned to 0 throughout: this test targets the AST-path alias marker.
-- On the serialized-plan path the header is reconciled by name regardless of the marker, so the
-- marker_off swap below does not occur there; the "distributed plan" CI flavor would otherwise
-- force the plan path on and the marker_off query would succeed instead of erroring.
SELECT 'marker_on';
SELECT x, a_str, inner_c
FROM t_se_outer
ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 0
FORMAT TSVWithNames;

SELECT 'marker_off';
SELECT x, a_str, inner_c
FROM t_se_outer
ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 0, prefer_localhost_replica = 0, serialize_query_plan = 0
FORMAT TSVWithNames;

DROP TABLE t_se_outer;
DROP TABLE t_se_inner;
DROP TABLE t_se_local;
