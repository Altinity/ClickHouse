-- The non-replicated deduplication window writes an on-disk deduplication log
-- (deduplication_logs/deduplication_log_N.txt) at the table root. On a content-addressed disk that
-- log auto-detects the disk does not support append writes (just like a plain s3 disk) and rewrites a
-- fresh rotated log object per record. DETACH+ATTACH reloads the log from disk, so a block inserted
-- before the reload is still deduplicated afterwards — proving the log persisted, not just the
-- in-memory window.

DROP TABLE IF EXISTS t_04298;

CREATE TABLE t_04298 (k UInt64)
ENGINE = MergeTree ORDER BY k
SETTINGS non_replicated_deduplication_window = 100;

-- Identical inserts are deduplicated through the in-memory window (and each writes a log record).
INSERT INTO t_04298 VALUES (1);
INSERT INTO t_04298 VALUES (1);
SELECT 'after-two-identical', count() FROM t_04298;

-- Reload the table: the dedup log is re-read from disk.
DETACH TABLE t_04298;
ATTACH TABLE t_04298;

-- The same block is still deduplicated — the record came from the on-disk log, not memory.
INSERT INTO t_04298 VALUES (1);
SELECT 'after-reload-same-block', count() FROM t_04298;

-- A new block is accepted.
INSERT INTO t_04298 VALUES (2);
SELECT 'after-new-block', count() FROM t_04298;

DROP TABLE t_04298;
