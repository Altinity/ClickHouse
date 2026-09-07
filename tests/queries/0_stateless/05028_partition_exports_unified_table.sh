#!/usr/bin/env bash
# Tags: no-fasttest, no-shared-merge-tree
# no-fasttest: requires S3 / MinIO.
# no-shared-merge-tree: asserts that a plain MergeTree and a ReplicatedMergeTree export share one system table.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

mt_table="mt_table_${CLICKHOUSE_DATABASE}"
rmt_table="rmt_table_${CLICKHOUSE_DATABASE}"
s3_table="s3_table_${CLICKHOUSE_DATABASE}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

# Poll system.partition_exports until the export of the given source table reaches the expected status.
wait_for_status() {
    local source_table="$1"
    local expected="$2"
    local i=0
    while [ "$i" -lt 120 ]; do
        status=$(query "SELECT status FROM system.partition_exports WHERE source_table = '$source_table' AND destination_table = '$s3_table' AND partition_id = '2020'")
        if [ "$status" = "$expected" ]; then
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    echo "TIMEOUT waiting for the export of $source_table to reach $expected (last: '$status')"
    return 1
}

query "DROP TABLE IF EXISTS $mt_table"
query "DROP TABLE IF EXISTS $rmt_table"
query "DROP TABLE IF EXISTS $s3_table"

echo "Both names expose the same schema"
diff <(query "DESCRIBE TABLE system.partition_exports") <(query "DESCRIBE TABLE system.replicated_partition_exports") && echo "OK"

query "CREATE TABLE $mt_table (id UInt64, year UInt16) ENGINE = MergeTree PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $rmt_table (id UInt64, year UInt16) ENGINE = ReplicatedMergeTree('/clickhouse/tables/$CLICKHOUSE_DATABASE/$rmt_table', 'r1') PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $s3_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table', format=Parquet, partition_strategy='hive') PARTITION BY year"

query "INSERT INTO $mt_table VALUES (1, 2020)"
query "INSERT INTO $rmt_table VALUES (2, 2020)"

query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table"
wait_for_status "$mt_table" "COMPLETED"

query "ALTER TABLE $rmt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table"
wait_for_status "$rmt_table" "COMPLETED"

echo "Both engines are tracked in system.partition_exports"
query "SELECT replaceOne(source_table, '$CLICKHOUSE_DATABASE', '{db}'), status, parts_count, length(parts), empty(source_replica) FROM system.partition_exports WHERE source_table IN ('$mt_table', '$rmt_table') ORDER BY 1"

echo "The alias returns the same rows"
query "SELECT count() FROM (SELECT * FROM system.partition_exports WHERE source_table IN ('$mt_table', '$rmt_table') EXCEPT SELECT * FROM system.replicated_partition_exports WHERE source_table IN ('$mt_table', '$rmt_table'))"

# parts_to_do is only asserted for the plain MergeTree row: a Replicated*MergeTree task never
# decrements it, because the manifest znode holding the part list is written once at schedule time.
echo "Replicated-only columns of the plain MergeTree row"
query "SELECT parts_to_do, length(destination_file_paths), last_exception_per_replica, committed_metadata_file, committed_marker_file != '' FROM system.partition_exports WHERE source_table = '$mt_table'"

query "DROP TABLE IF EXISTS $mt_table"
query "DROP TABLE IF EXISTS $rmt_table"
query "DROP TABLE IF EXISTS $s3_table"
