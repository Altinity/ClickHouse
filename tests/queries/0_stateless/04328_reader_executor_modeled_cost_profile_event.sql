-- Tags: no-distributed-cache, no-encrypted-storage, no-cas-storage
-- The executor falls back on the distributed cache and decryption (which can't be
-- disabled from the test), so its metrics would not be emitted there; skip those
-- configs (as in 04316 / 04327). Content-addressed storage always adds a
-- `file_view` stage (byte window inside a shared blob), which the executor
-- falls back on the same way -- see `ReadPipeline::tryBuildReaderExecutor`.
--
-- End-to-end check that a `use_reader_executor` read records a non-zero modeled
-- cost in the `ReaderExecutorModeledCostMicroseconds` ProfileEvent.

DROP TABLE IF EXISTS t_reader_executor_kpi;

CREATE TABLE t_reader_executor_kpi
(
    id UInt64,
    v UInt64,
    s String
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 8192;

INSERT INTO t_reader_executor_kpi
SELECT number, number * 2, concat('row_', toString(number))
FROM numbers(300000);

SET use_reader_executor = 1;
SET remote_filesystem_read_method = 'read';
SET enable_filesystem_cache = 0;

-- The load. Its result is irrelevant (it only drives the executor).
SELECT count(), sum(id), sum(v), sum(length(s)) FROM t_reader_executor_kpi
SETTINGS log_comment = '04328_reader_executor_modeled_cost_probe' FORMAT Null;

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['ReaderExecutorModeledCostMicroseconds'] > 0
FROM system.query_log
WHERE log_comment = '04328_reader_executor_modeled_cost_probe'
  AND type = 'QueryFinish'
  AND current_database = currentDatabase()
ORDER BY event_time_microseconds DESC
LIMIT 1;

DROP TABLE t_reader_executor_kpi;
