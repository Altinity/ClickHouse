#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-replicated-database, replica
# Tag no-fasttest: requires S3 storage.
# An already-exported partition must not be re-exported on the next scheduler tick.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

src="ttl_export_skip_src_${RANDOM}"
dst="ttl_export_skip_dst_${RANDOM}"

query() { $CLICKHOUSE_CLIENT --query "$1"; }

poll_status() {
    local partition="$1"
    local expected="$2"
    local deadline=$(( $(date +%s) + 90 ))
    local s=""
    while [ "$(date +%s)" -lt "$deadline" ]; do
        s=$(query "SELECT status FROM system.replicated_partition_exports WHERE source_table = '$src' AND destination_table = '$dst' AND partition_id = '$partition'")
        if [ "$s" = "$expected" ]; then return 0; fi
        sleep 1
    done
    echo "Timed out waiting for partition $partition to reach $expected (last: '$s')" >&2
    return 1
}

query "DROP TABLE IF EXISTS $src SYNC"
query "DROP TABLE IF EXISTS $dst SYNC"

query "CREATE TABLE $dst (event_date Date, id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$dst', format=Parquet, partition_strategy='hive') PARTITION BY year"

# Identity partition key on `year` so the hive S3 destination accepts it.
query "CREATE TABLE $src (event_date Date, id UInt64, year UInt16)
       ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/$src', 'r1')
       PARTITION BY year ORDER BY id
       TTL event_date + INTERVAL 1 DAY EXPORT TO TABLE $dst"

query "INSERT INTO $src VALUES (toDate('2000-01-01'), 1, 2000), (toDate('2000-01-02'), 2, 2000)"
poll_status 2000 COMPLETED

echo "---- after first export, destination row count"
query "SELECT count() FROM $dst"

# Add a second partition; it should export and the first must not export again.
query "INSERT INTO $src VALUES (toDate('2001-01-01'), 3, 2001)"
poll_status 2001 COMPLETED

echo "---- after second export, destination row count"
query "SELECT count() FROM $dst"

# The destination row count for partition 2000 is the witness that it was not re-exported:
# the ttl-marker for 2000 is removed when 2001 is submitted, so the history-count check is
# no longer meaningful under the new "at most one ttl-origin per (src, dest)" invariant.
echo "---- partition 2000 has no duplicate rows in destination"
query "SELECT count() FROM $dst WHERE year = 2000"

query "DROP TABLE $src SYNC"
query "DROP TABLE $dst SYNC"
