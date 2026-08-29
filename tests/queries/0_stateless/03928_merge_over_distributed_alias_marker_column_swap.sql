-- Plain Merge over Distributed over MergeTree (no Hybrid, no explicit __aliasMarker).
-- Nested ALIAS columns (b contains a's subexpression). Reading the alias columns through the
-- Merge table must reconcile the child (Distributed) header by name; a positional reconciliation
-- in StorageMerge::convertAndFilterSourceStream would swap the columns (or fill them with 0).
-- The correct result equals the single-node ('local') result.
--
-- Determinism notes: `x` is kept in GROUP BY so the ALIAS expansion can resolve it (the alias
-- expressions are defined in terms of x); GROUP BY also deduplicates the rows the two shards
-- produce, and ORDER BY x (distinct values) gives a total order independent of the distributed
-- merge order. So every block - local and the distributed variants - yields the same rows.
DROP TABLE IF EXISTS test_merge_alias_swap_merge;
DROP TABLE IF EXISTS test_merge_alias_swap_dist;
DROP TABLE IF EXISTS test_merge_alias_swap_local;

CREATE TABLE test_merge_alias_swap_local
(
    x UInt64,
    a UInt64 ALIAS x + 1,
    b UInt64 ALIAS a + 1
)
ENGINE = MergeTree()
ORDER BY x;

INSERT INTO test_merge_alias_swap_local VALUES (1), (2), (10);

CREATE TABLE test_merge_alias_swap_dist AS test_merge_alias_swap_local
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_merge_alias_swap_local);

CREATE TABLE test_merge_alias_swap_merge
(
    x UInt64,
    a UInt64,
    b UInt64
)
ENGINE = Merge(currentDatabase(), '^test_merge_alias_swap_dist$');

SELECT 'local';
SELECT x, a, b FROM test_merge_alias_swap_local GROUP BY x, a, b ORDER BY x;

SELECT 'merge_prefer0';
SELECT x, a, b FROM test_merge_alias_swap_merge GROUP BY x, a, b ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0;

SELECT 'merge_prefer1';
SELECT x, a, b FROM test_merge_alias_swap_merge GROUP BY x, a, b ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1;

SELECT 'merge_prefer0_plan';
SELECT x, a, b FROM test_merge_alias_swap_merge GROUP BY x, a, b ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 0, serialize_query_plan = 1;

SELECT 'merge_prefer1_plan';
SELECT x, a, b FROM test_merge_alias_swap_merge GROUP BY x, a, b ORDER BY x
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, prefer_localhost_replica = 1, serialize_query_plan = 1;

DROP TABLE test_merge_alias_swap_merge;
DROP TABLE test_merge_alias_swap_dist;
DROP TABLE test_merge_alias_swap_local;
