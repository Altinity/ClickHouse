#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: depends on MinIO via s3_conn.

# `object_storage_max_files_to_prefetch` must never change which rows a scan returns, only when
# their background reads start. Raising it lets one reading stream have several tasks in flight
# racing for the shared file iterator, so a lookahead slot queued earlier can resolve to no file
# while later-queued slots already hold files; the stream must still read every one of them.
#
# The failure this guards against is a silently short result, and it is racy: a broken build
# returns the right answer a good fraction of the time. Each configuration is therefore checked
# repeatedly - a single run per setting would let a regression through roughly half the time.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

path="04626_prefetch_${CLICKHOUSE_DATABASE}"

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS test_04626_write"
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE test_04626_write (id UInt64, payload String)
    ENGINE = S3(s3_conn, filename = '$path/file_{_partition_id}.parquet', format = Parquet, partition_strategy = 'wildcard')
    PARTITION BY id % 6"

# 600 rows spread over 6 files, 100 rows each, so a lost file shows up as a short count.
$CLICKHOUSE_CLIENT --s3_truncate_on_insert 1 --query "
    INSERT INTO test_04626_write SELECT number AS id, repeat('x', 16) AS payload FROM numbers(600)"

$CLICKHOUSE_CLIENT --query "DROP TABLE test_04626_write"

# Depth 1 is the default (nothing is primed ahead), 3 and 8 put several tasks in flight at once.
# 8 is deliberately larger than the 6 files that exist, so the lookahead queue can never fill and
# has to drain correctly once the file iterator runs out mid-refill.
for depth in 1 3 8; do
    for _ in {1..10}; do
        $CLICKHOUSE_CLIENT --query "
            SELECT sum(id), count(), sum(sipHash64(payload))
            FROM s3(s3_conn, filename = '$path/file_*.parquet', format = Parquet)
            SETTINGS max_threads = 2, object_storage_max_files_to_prefetch = $depth, enable_filesystem_cache = 0"
    done
done

# Row *order* is deliberately not asserted: with several tasks racing for the file iterator, which
# file lands in which lookahead slot is not deterministic, so only the set of rows is guaranteed.
$CLICKHOUSE_CLIENT --query "
    SELECT arraySort(groupArray(id)) = (
        SELECT arraySort(groupArray(id))
        FROM s3(s3_conn, filename = '$path/file_*.parquet', format = Parquet)
        SETTINGS max_threads = 1, object_storage_max_files_to_prefetch = 1)
    FROM s3(s3_conn, filename = '$path/file_*.parquet', format = Parquet)
    SETTINGS max_threads = 1, object_storage_max_files_to_prefetch = 4, enable_filesystem_cache = 0"
