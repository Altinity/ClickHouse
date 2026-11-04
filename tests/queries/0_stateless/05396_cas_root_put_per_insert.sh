#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# Root-object PUTs of one INSERT into a warm namespace on a content-addressed disk: the manifest, the
# ref-log append that publishes the part under its temporary name, and the ref-log append that moves it
# to its final name. Pinned so a change in the publish protocol shows up as a number, not as latency.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

D="${CLICKHOUSE_DATABASE}_rp1"
DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${D}', name = '${D}', path = '${CLICKHOUSE_USER_FILES_UNIQUE}_rp1/', cas_gc_enabled = 0)"

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_rp1 SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_rp1 (a UInt64, v UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}, min_bytes_for_full_part_storage = 0"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_rp1"
# The first insert creates the namespace; the measured ones land in a warm one.
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_rp1 SELECT number, number FROM numbers(1000)"
for i in 1 2 3; do
    ${CLICKHOUSE_CLIENT} -q "INSERT INTO t_rp1 SELECT number, number FROM numbers(1000) SETTINGS log_comment = '${D}_insert_${i}'"
done
${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log"
${CLICKHOUSE_CLIENT} -q "SELECT 'root_put_per_insert', groupUniqArray(ProfileEvents['CASRootPut']), count()
    FROM system.query_log
    WHERE type = 'QueryFinish' AND current_database = currentDatabase() AND log_comment LIKE '${D}_insert_%'"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_rp1 SYNC"
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal -q "SYSTEM CAS FORGET '${D}'"
rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE:?}_rp1"
