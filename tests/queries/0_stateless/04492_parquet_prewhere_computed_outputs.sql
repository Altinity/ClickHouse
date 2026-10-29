-- Tags: no-fasttest
SET input_format_parquet_use_native_reader_v3 = 1;
SET optimize_move_to_prewhere = 1;
SET query_plan_optimize_prewhere = 1;
SET enable_analyzer = 1;

INSERT INTO FUNCTION file(concat(currentDatabase(), '/04492_prewhere.parquet'), Parquet)
SELECT
    toUInt8(number % 2) AS flag,
    if(number % 3 = 0, 'X', 'Y') AS kind,
    toInt64(1000 + number) AS a,
    toInt64(100 + number % 7) AS b,
    toInt64(number % 5) AS c,
    toInt64(50 + number) AS d
FROM numbers(1000)
SETTINGS engine_file_truncate_on_insert = 1;

-- The reader must return the computed PREWHERE output, not a missing-column default.
WITH (flag = 0) AND (kind = 'X') AS s, if(s, a - b, d) AS lat
SELECT count(), max(lat)
FROM file(concat(currentDatabase(), '/04492_prewhere.parquet'), Parquet)
WHERE ((NOT s) OR (c != 0)) AND (lat > 0);

WITH (flag = 0) AND (kind = 'X') AS s, if(s, a - b, d) AS lat
SELECT count(), max(lat)
FROM file(concat(currentDatabase(), '/04492_prewhere.parquet'), Parquet)
WHERE ((NOT s) OR (c != 0)) AND (lat > 0)
SETTINGS input_format_parquet_allow_missing_columns = 0;

WITH (flag = 0) AND (kind = 'X') AS s, if(s, a - b, d) AS lat
SELECT count(), max(lat)
FROM file(concat(currentDatabase(), '/04492_prewhere.parquet'), Parquet)
WHERE ((NOT s) OR (c != 0)) AND (lat > 0)
SETTINGS optimize_move_to_prewhere = 0;

-- Non-nullable outputs used to be silently replaced with zero instead of NULL.
WITH (flag = 0) AND (kind = 'X') AS s, if(s, a - b, d) AS lat
SELECT count(), max(lat)
FROM file(concat(currentDatabase(), '/04492_prewhere.parquet'), Parquet,
    'flag UInt8, kind String, a Int64, b Int64, c Int64, d Int64')
WHERE ((NOT s) OR (c != 0)) AND (lat > 0);

-- Exercise computed outputs together with Antalya's two-step row-policy filtering.
DROP ROW POLICY IF EXISTS p_computed ON t_prewhere_computed;
DROP TABLE IF EXISTS t_prewhere_computed;
CREATE TABLE t_prewhere_computed
    (flag UInt8, kind String, a Int64, b Int64, c Int64, d Int64)
ENGINE = File(Parquet);
INSERT INTO t_prewhere_computed
SELECT * FROM file(concat(currentDatabase(), '/04492_prewhere.parquet'), Parquet);
CREATE ROW POLICY p_computed ON t_prewhere_computed FOR SELECT USING flag TO ALL;

WITH (flag = 0) AND (kind = 'X') AS s, if(s, a - b, d) AS lat
SELECT count(), max(lat) FROM t_prewhere_computed
PREWHERE ((NOT s) OR (c != 0)) AND (lat > 0);

WITH (flag = 0) AND (kind = 'X') AS s, if(s, a - b, d) AS lat
SELECT count(), max(lat) FROM t_prewhere_computed
WHERE ((NOT s) OR (c != 0)) AND (lat > 0);

DROP ROW POLICY p_computed ON t_prewhere_computed;
DROP TABLE t_prewhere_computed;

-- A missing physical column passed through PREWHERE must still get its default.
SET input_format_parquet_allow_missing_columns = 1;
SET input_format_parquet_filter_push_down = 1;
INSERT INTO FUNCTION file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet)
SELECT toInt64(number) AS x FROM numbers(4)
SETTINGS engine_file_truncate_on_insert = 1;

SELECT x, y
FROM file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet, 'x Int64, y Nullable(Int64)')
WHERE y IS NULL ORDER BY x;

SELECT y
FROM file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet, 'x Int64, y Nullable(Int64)')
PREWHERE y IS NULL;

SELECT x, y
FROM file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet, 'x Int64, y Int64')
WHERE y = 0 AND x < 3 ORDER BY x;

-- Disabling min/max pruning must not merely move the failure into preparePrewhere.
SELECT x, y
FROM file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet, 'x Int64, y Nullable(Int64)')
WHERE y IS NULL ORDER BY x
SETTINGS input_format_parquet_filter_push_down = 0;

SELECT y
FROM file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet, 'x Int64, y Nullable(Int64)')
PREWHERE y IS NULL
SETTINGS input_format_parquet_filter_push_down = 0;

SELECT x, y
FROM file(concat(currentDatabase(), '/04492_missing.parquet'), Parquet, 'x Int64, y Int64')
WHERE y = 0 AND x < 3 ORDER BY x
SETTINGS input_format_parquet_filter_push_down = 0;
