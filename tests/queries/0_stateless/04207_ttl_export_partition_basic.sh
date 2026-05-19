#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-replicated-database, replica
# Tag no-fasttest: requires S3 storage.
# Happy path for TTL EXPORT: expired partitions show up in destination, source data is preserved.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

src="ttl_export_basic_src_${RANDOM}"
dst="ttl_export_basic_dst_${RANDOM}"

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

# Source carries the EXPORT TTL from creation, so parts get export_ttl info populated at write time.
# This avoids the MATERIALIZE TTL requirement that applies to parts predating the TTL.
# Identity partition key on `year` so the hive S3 destination accepts it.
query "CREATE TABLE $src (event_date Date, id UInt64, year UInt16)
       ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/$src', 'r1')
       PARTITION BY year ORDER BY id
       TTL event_date + INTERVAL 1 DAY EXPORT TO $dst"

query "INSERT INTO $src VALUES (toDate('2000-01-01'), 1, 2000), (toDate('2000-01-02'), 2, 2000), (toDate('2000-01-03'), 3, 2000)"
query "INSERT INTO $src VALUES (toDate('2001-01-01'), 4, 2001), (toDate('2001-01-02'), 5, 2001)"
# Far-future partition: must remain unexported because `event_date + INTERVAL 1 DAY` is well past now().
query "INSERT INTO $src VALUES (toDate('2100-01-01'), 99, 2100)"

# Only the trailing partition is guaranteed to be observable as COMPLETED — the 2000
# ttl-marker is removed when 2001 is submitted ("at most one ttl-origin per (src, dest)").
# The destination row check below confirms that both partitions actually exported.
poll_status 2001 COMPLETED

# Give the scheduler one more tick after 2001 completes so a buggy "ignore the `< now()` check"
# implementation has the chance to pick 2100 up. Default poll interval is 5s + ~25% jitter.
sleep 8

echo "---- destination contents"
query "SELECT id, event_date FROM $dst ORDER BY id"

echo "---- source still has the data (EXPORT does not drop locally)"
query "SELECT count() FROM $src"

echo "---- not-yet-expired partition 2100 is absent from destination and from history"
query "SELECT count() FROM $dst WHERE year = 2100"
query "SELECT count() FROM system.replicated_partition_exports WHERE source_table = '$src' AND destination_table = '$dst' AND partition_id = '2100'"

echo "---- no BAD_ARGUMENTS attributed to the scheduler"
query "SELECT count() FROM system.errors WHERE name = 'BAD_ARGUMENTS' AND last_error_message ILIKE '%${src}%'"

query "DROP TABLE $src SYNC"
query "DROP TABLE $dst SYNC"
