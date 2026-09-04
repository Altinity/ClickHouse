#!/usr/bin/env bash
# Tags: no-fasttest, no-shared-merge-tree
# no-fasttest: requires S3 / MinIO.
# no-shared-merge-tree: this test exercises EXPORT PARTITION on a plain (non-replicated) MergeTree.

# Dots are legal in identifiers, so the destinations `<db>.x`.`y` and `<db>`.`x.y` flatten to the
# same qualified name. The export registry must still track them as two independent tasks.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

mt_table="mt_table_${CLICKHOUSE_DATABASE}"
dotted_database="${CLICKHOUSE_DATABASE}.x"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

# Poll system.partition_exports until the export to the given destination reaches the expected status.
wait_for_status() {
    local destination_database="$1"
    local destination_table="$2"
    local expected="$3"
    local i=0
    while [ "$i" -lt 120 ]; do
        status=$(query "SELECT status FROM system.partition_exports WHERE source_table = '$mt_table' AND destination_database = '$destination_database' AND destination_table = '$destination_table' AND partition_id = '2020'")
        if [ "$status" = "$expected" ]; then
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    echo "TIMEOUT waiting for export to $destination_database.$destination_table to reach $expected (last: '$status')"
    return 1
}

query "DROP DATABASE IF EXISTS \`$dotted_database\`"
query "CREATE DATABASE \`$dotted_database\`"

query "CREATE TABLE $mt_table (id UInt64, year UInt16) ENGINE = MergeTree PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE \`$dotted_database\`.y (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='first_${CLICKHOUSE_DATABASE}', format=Parquet, partition_strategy='hive') PARTITION BY year"
query "CREATE TABLE $CLICKHOUSE_DATABASE.\`x.y\` (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='second_${CLICKHOUSE_DATABASE}', format=Parquet, partition_strategy='hive') PARTITION BY year"

query "INSERT INTO $mt_table VALUES (1, 2020), (2, 2020)"

echo "Export partition 2020 to the first destination"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE \`$dotted_database\`.y"
wait_for_status "$dotted_database" "y" "COMPLETED"

echo "The same partition can be exported to the second destination"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $CLICKHOUSE_DATABASE.\`x.y\`"
wait_for_status "$CLICKHOUSE_DATABASE" "x.y" "COMPLETED"

echo "Both exports are tracked independently"
query "SELECT replaceOne(destination_database, '$CLICKHOUSE_DATABASE', '{db}'), destination_table, status FROM system.partition_exports WHERE source_table = '$mt_table' ORDER BY 1, 2"

echo "Both destinations received the partition"
query "SELECT * FROM \`$dotted_database\`.y ORDER BY id"
query "SELECT * FROM $CLICKHOUSE_DATABASE.\`x.y\` ORDER BY id"

echo "Re-exporting to the first destination is still rejected"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE \`$dotted_database\`.y" 2>&1 | grep -o "EXPORT_PARTITION_ALREADY_EXPORTED" | head -1

query "DROP TABLE IF EXISTS $CLICKHOUSE_DATABASE.\`x.y\`"
query "DROP TABLE IF EXISTS $mt_table"
query "DROP DATABASE IF EXISTS \`$dotted_database\`"
