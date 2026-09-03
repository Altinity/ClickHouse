-- https://github.com/ClickHouse/ClickHouse/issues/49706
-- `FORMAT Null` skips client-side deserialization, so discard `RowBinary` output instead.
SELECT
    topKResampleState(1048576, 257, 65536, 10)(toString(number), number)
FROM numbers(3)
INTO OUTFILE '/dev/null' TRUNCATE FORMAT RowBinary;
