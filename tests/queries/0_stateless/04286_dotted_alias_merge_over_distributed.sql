-- Regression for the StorageMerge alias-output-naming fix.
-- The bug: `Nested::splitName(name, reverse=true)` (used before this fix to strip the
-- analyzer's `__tableN.` prefix from header column names) splits on the LAST dot, so for
-- an analyzer identifier like `__table1.\`n.a\`` (a dotted column name wrapped in backticks
-- by the analyzer) it returns the suffix `a\`` instead of `n.a`, leaving the
-- `logical_name_to_header_name` map with broken keys. The lookup for `alias.name == "n.a"`
-- then misses, the alias output falls back to the bare name `n.a`, and the downstream
-- header-reconciliation step fills the expected `__table1.\`n.a\`` column with type
-- defaults (zeros). Silent wrong data.
--
-- Repro shape: Merge declares dotted column names explicitly (typical when matching a
-- schema with Nested-style names), underlying storage has those columns as ALIAS, and
-- the Distributed routing forces analyzer-prefixed names in the Merge level. Using a
-- two-shard cluster with prefer_localhost_replica=0 reliably reproduces.

DROP TABLE IF EXISTS test_04286_dotted_alias_local;
DROP TABLE IF EXISTS test_04286_dotted_alias_dist;
DROP TABLE IF EXISTS test_04286_dotted_alias_merge;

CREATE TABLE test_04286_dotted_alias_local
(
    id UInt32,
    `n.a` UInt32 ALIAS id * 10,
    `m.b` UInt32 ALIAS id * 100
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO test_04286_dotted_alias_local VALUES (1), (2);

CREATE TABLE test_04286_dotted_alias_dist AS test_04286_dotted_alias_local
ENGINE = Distributed(test_cluster_two_shards, currentDatabase(), test_04286_dotted_alias_local);

CREATE TABLE test_04286_dotted_alias_merge
(
    id UInt32,
    `n.a` UInt32,
    `m.b` UInt32
)
ENGINE = Merge(currentDatabase(), '^test_04286_dotted_alias_dist$');

SELECT 'local';
SELECT id, `n.a`, `m.b`
FROM test_04286_dotted_alias_local
GROUP BY id, `n.a`, `m.b`
ORDER BY id;

SELECT 'merge_prefer0';
SELECT id, `n.a`, `m.b`
FROM test_04286_dotted_alias_merge
GROUP BY id, `n.a`, `m.b`
ORDER BY id
SETTINGS prefer_localhost_replica = 0;

SELECT 'merge_prefer1';
SELECT id, `n.a`, `m.b`
FROM test_04286_dotted_alias_merge
GROUP BY id, `n.a`, `m.b`
ORDER BY id
SETTINGS prefer_localhost_replica = 1;

DROP TABLE test_04286_dotted_alias_merge;
DROP TABLE test_04286_dotted_alias_dist;
DROP TABLE test_04286_dotted_alias_local;
