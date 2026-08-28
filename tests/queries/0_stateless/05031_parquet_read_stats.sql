-- Tags: no-fasttest, no-random-settings

DROP TABLE IF EXISTS t_parquet_read_stats;

CREATE TABLE t_parquet_read_stats (a Int64, s String)
ENGINE = S3(s3_conn, filename='test_05031_parquet_read_stats', format='Parquet');

-- Two row groups of a few MB each, so the prefetcher issues at least one real source read that the
-- progress callback (and hence `Prefetcher::runTask`'s first-byte/transfer timing) has a chance to
-- fire for.
INSERT INTO t_parquet_read_stats
    SELECT number, randomString(200)
    FROM system.numbers
    LIMIT 100000
SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 50000;

-- sum(length(s)) forces the reader to actually decode the `s` column (a plain `count()` can be
-- answered from row group metadata alone, without reading any column data through the prefetcher).
SELECT count(), sum(length(s))
FROM t_parquet_read_stats
SETTINGS log_comment = 'test_05031_parquet_read_stats', use_parquet_metadata_cache = 0;

SYSTEM FLUSH LOGS query_log;

-- Sanity-bound the two new events against the query's own wall-clock duration, generously scaled up
-- (reads from multiple tasks/threads can sum to more than one wall-clock duration's worth of
-- microseconds, and MinIO round-trips are fast) so this stays deterministic while still catching a
-- units/overflow-class bug (e.g. a bogus bandwidth sample turning a normal read into a bogus,
-- wildly larger stored time) rather than a plain non-negativity check that any value satisfies.
SELECT
    ProfileEvents['ParquetReadFirstByteMicroseconds'] > 0,
    ProfileEvents['ParquetReadFirstByteMicroseconds'] + ProfileEvents['ParquetReadTransferMicroseconds'] <= (query_duration_ms + 1) * 1000 * 100
FROM system.query_log
WHERE type = 'QueryFinish' AND event_date >= yesterday() AND event_time >= now() - 600 AND query_kind = 'Select' AND current_database = currentDatabase()
    AND log_comment = 'test_05031_parquet_read_stats'
ORDER BY event_time DESC
LIMIT 1;

DROP TABLE IF EXISTS t_parquet_read_stats;
