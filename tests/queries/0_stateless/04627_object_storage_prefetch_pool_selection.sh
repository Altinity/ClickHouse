#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel
# no-fasttest: depends on MinIO via s3_conn.
# no-parallel: `object_storage_reader_pool_pause` is a process-wide PAUSEABLE failpoint. While it is
#   enabled it would also pause background reader creation in any other object storage query
#   running concurrently in a parallel test session.
#
# Regression test: at the default `object_storage_max_files_to_prefetch` (1), background reader
# creation must run on a dedicated single-thread pool (`StorageObjectStorageThreads*`), not on the
# shared, server-wide prefetch pool (`IOPrefetchThreads*`) that `MergeTreePrefetchedReadPool` also
# submits to - otherwise this source's prefetches contend with unrelated queries on that pool.
# Above 1, a private pool sized from the setting would make thread count unbounded
# (streams * max_files_to_prefetch), so the shared pool is used instead; that direction is checked
# too, so a change that always uses one pool regardless of the setting cannot pass either half.
#
# The pool is exercised even at the default of 1 (see `refillReaderFutures`): a lookahead future for
# the next file is always queued via `createReaderAsync`, just left unprimed. A pauseable failpoint
# at the top of that scheduled task holds it on whichever pool it was submitted to for long enough
# to read `system.metrics` and see which pool went active.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

path="04627_prefetch_pool_${CLICKHOUSE_DATABASE}"

function cleanup()
{
    $CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT object_storage_reader_pool_pause" 2>/dev/null ||:
}
trap cleanup EXIT

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS test_04627_write"
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE test_04627_write (id UInt64)
    ENGINE = S3(s3_conn, filename = '$path/file.parquet', format = Parquet)"
$CLICKHOUSE_CLIENT --query "INSERT INTO test_04627_write SELECT number FROM numbers(10)"
$CLICKHOUSE_CLIENT --query "DROP TABLE test_04627_write"

function metric_value()
{
    $CLICKHOUSE_CLIENT --query "SELECT value FROM system.metrics WHERE metric = '$1'"
}

# $1: object_storage_max_files_to_prefetch value to test
# $2: metric expected to go active (the pool this setting should route through)
# $3: metric expected to stay at its pre-query baseline (the pool it should NOT use)
function check_pool()
{
    local depth=$1 expect_active=$2 expect_idle=$3
    local idle_baseline active idle
    idle_baseline=$(metric_value "$expect_idle")

    $CLICKHOUSE_CLIENT -q "SYSTEM ENABLE FAILPOINT object_storage_reader_pool_pause"

    $CLICKHOUSE_CLIENT --query "
        SELECT count() FROM s3(s3_conn, filename = '$path/file.parquet', format = Parquet)
        SETTINGS max_threads = 1, object_storage_max_files_to_prefetch = $depth" &
    local q_pid=$!

    $CLICKHOUSE_CLIENT -q "SYSTEM WAIT FAILPOINT object_storage_reader_pool_pause PAUSE"

    active=$(metric_value "$expect_active")
    idle=$(metric_value "$expect_idle")

    if [[ "$active" -lt 1 ]]; then
        echo "FAIL depth=$depth: expected $expect_active active, got $active"
    fi
    if [[ "$idle" -ne "$idle_baseline" ]]; then
        echo "FAIL depth=$depth: expected $expect_idle to stay at $idle_baseline, got $idle"
    fi

    $CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT object_storage_reader_pool_pause"
    wait "$q_pid"
}

check_pool 1 StorageObjectStorageThreadsActive IOPrefetchThreadsActive
check_pool 4 IOPrefetchThreadsActive StorageObjectStorageThreadsActive

echo "OK"
