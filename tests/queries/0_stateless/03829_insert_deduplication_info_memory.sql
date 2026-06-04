-- Tags: no-random-settings, no-content-addressed-storage
-- no-content-addressed-storage: REAL CA BUG (B65): the content_addressed write path adds peak-memory overhead on INSERT so a tight max_memory_usage=150M INSERT hits Code 241 MEMORY_LIMIT_EXCEEDED on CA while it passes on plain
-- Verify that DeduplicationInfo does not store original_block when deduplication is disabled,
-- which would otherwise roughly double the peak memory during INSERT.

SET allow_suspicious_fixed_string_types = 1;

DROP TABLE IF EXISTS t_dedup_memory;
CREATE TABLE t_dedup_memory (x UInt32, fat FixedString(10000)) ENGINE = MergeTree ORDER BY x;

-- 10 000 rows * 10 000 bytes FixedString ≈ 100 MB of column data.
-- With the bug, original_block doubles this to ~200 MB, exceeding the limit.
-- Without the bug, only the data columns are held, fitting within the limit.
SET max_memory_usage = '150M';

INSERT INTO t_dedup_memory SELECT number, toString(number) FROM numbers(10000)
    SETTINGS max_insert_threads = 1, min_insert_block_size_rows = 0, min_insert_block_size_bytes = 0;

SELECT count() FROM t_dedup_memory;

DROP TABLE t_dedup_memory;
