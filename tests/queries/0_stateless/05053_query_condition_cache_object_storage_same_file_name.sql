-- Tags: no-fasttest
-- Tag no-fasttest: Depends on AWS

-- The query condition cache is keyed per file. A Hive-partitioned dataset repeats the same file name in
-- every partition, so keying by name alone would make all of them share one entry: the first partition
-- that matches nothing marks its namesakes as having no matching row groups, and every later query skips
-- them unread. Reading the same table twice must return the same rows.

SET s3_truncate_on_insert = 1;
SET use_query_condition_cache = 1;

INSERT INTO FUNCTION s3(s3_conn, filename='05053_qcc/day=2025-02-27/part-00000.parquet', format=Parquet)
    SELECT toDateTime('2025-02-27 00:00:00') + number AS ts, number AS v FROM numbers(1000);

INSERT INTO FUNCTION s3(s3_conn, filename='05053_qcc/day=2025-02-05/part-00000.parquet', format=Parquet)
    SELECT toDateTime('2025-02-05 00:00:00') + number AS ts, number AS v FROM numbers(1000);

DROP TABLE IF EXISTS t_05053_qcc;

-- A table, not a table function: the cache entry only outlives a query when the table id is stable.
CREATE TABLE t_05053_qcc (ts DateTime, v UInt32)
    ENGINE = S3(s3_conn, filename='05053_qcc/**.parquet', format=Parquet);

SELECT count() FROM t_05053_qcc WHERE ts >= '2025-02-05 00:00:00' AND ts < '2025-02-06 00:00:00';
SELECT count() FROM t_05053_qcc WHERE ts >= '2025-02-05 00:00:00' AND ts < '2025-02-06 00:00:00';
SELECT count() FROM t_05053_qcc WHERE ts >= '2025-02-05 00:00:00' AND ts < '2025-02-06 00:00:00';

DROP TABLE t_05053_qcc;
