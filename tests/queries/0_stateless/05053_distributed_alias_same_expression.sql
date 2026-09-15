-- Two ALIAS columns over the same expression, read through `remote` with ORDER BY. The shard's
-- ActionsDAG deduplicates the two identical expressions into one output column, so its header is a
-- column short of what the initiator expects.
--
-- Every variant below must return the same single row. `enable_alias_marker = 0` is covered too:
-- the marker keeps the two columns distinct in transport, but it is not what makes this shape work.
-- `buildShardCollapseFanOut` reconstructs the missing column either way, so turning the marker off
-- must not change the answer.
--
-- Related issue: https://github.com/ClickHouse/ClickHouse/issues/79916
-- Fixed upstream by: https://github.com/ClickHouse/ClickHouse/pull/107913

DROP TABLE IF EXISTS test_alias_same_expr_remote;

CREATE TABLE test_alias_same_expr_remote
(
    dt DateTime64(3),
    String_7 String,
    alias_String_7_0 String ALIAS String_7,
    alias_String_7_1 String ALIAS String_7
)
ENGINE = MergeTree()
ORDER BY dt;

INSERT INTO test_alias_same_expr_remote VALUES ('1999-03-29T01:15:33', '');

SELECT 'first';
SELECT dt, alias_String_7_0, alias_String_7_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
LIMIT 1;

SELECT 'second';
SELECT dt, alias_String_7_0, alias_String_7_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 0;

SELECT 'third';
SELECT dt, alias_String_7_0, alias_String_7_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 1;

SELECT 'fourth';
SELECT dt, alias_String_7_0, alias_String_7_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 1, enable_alias_marker = 0;

SELECT 'fifth';
SELECT dt, alias_String_7_0, alias_String_7_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, serialize_query_plan = 1;

SELECT 'sixth';
SELECT alias_String_7_0 AS query_alias_0, alias_String_7_1 AS query_alias_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1
FORMAT TSVWithNames;

SELECT 'seventh';
SELECT alias_String_7_0, alias_String_7_1
FROM remote('127.0.0.{1,2}', currentDatabase(), test_alias_same_expr_remote)
ORDER BY dt
LIMIT 1
SETTINGS enable_analyzer = 1, enable_alias_marker = 1
FORMAT TSVWithNames;

DROP TABLE test_alias_same_expr_remote;
