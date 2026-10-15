-- Tags: no-fasttest

SET input_format_parquet_use_native_reader_v3 = 1;
SET input_format_parquet_filter_push_down = 0;
SET input_format_parquet_page_filter_push_down = 0;
SET input_format_parquet_bloom_filter_push_down = 0;

DROP TABLE IF EXISTS parquet_prewhere_numeric_filter;
CREATE TABLE parquet_prewhere_numeric_filter
(
    id UInt32,
    value Int64,
    nullable_value Nullable(Int64)
)
ENGINE = File(Parquet);

INSERT INTO parquet_prewhere_numeric_filter VALUES
    (1, 0, NULL), (2, 256, 256), (3, -256, -256), (4, 2147483648, 0);

-- The AST fuzzer found a bad UInt32-to-UInt8 column cast for this constant.
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE 2147483648 WHERE indexHint(id != 0) ORDER BY id;
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE 0 WHERE indexHint(id != 0) ORDER BY id;

-- Converting to UInt8 must test truthiness, not truncate the low eight bits.
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE value ORDER BY id;
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE nullable_value ORDER BY id;
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE toFloat64(value) / 1024 ORDER BY id;

-- Keep coverage of the existing UInt8 and Nullable(UInt8) fast paths.
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE value != 0 ORDER BY id;
SELECT id FROM parquet_prewhere_numeric_filter PREWHERE nullable_value != 0 ORDER BY id;

DROP TABLE parquet_prewhere_numeric_filter;
