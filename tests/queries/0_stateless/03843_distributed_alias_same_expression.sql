-- Regression coverage for distributed ORDER BY + ALIAS columns with identical expressions.
-- Related issue: https://github.com/ClickHouse/ClickHouse/issues/79916

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
SETTINGS enable_analyzer = 1, enable_alias_marker = 0; -- { serverError NUMBER_OF_COLUMNS_DOESNT_MATCH }

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
