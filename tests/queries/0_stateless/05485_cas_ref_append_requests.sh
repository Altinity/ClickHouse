#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image. Randomized settings could
#   move the requests out of the measured query.

# An ordinary ref-log append is one create of `_log/{epoch,seq}`: no `_ckpt` write, no read of `_ckpt` or of a
# log. Per query: `_ckpt` writes, `_ckpt` and log reads, root `PUT`s and other `GET`s. A query whose requests
# were not counted prints zero root `PUT`s and fails the reference.
# The first insert creates the namespace and is not measured.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="${CLICKHOUSE_DATABASE}_05485_cas"
DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05485',
    cas_gc_enabled = 0,
    name = '${DISK_NAME}',
    path = '${CLICKHOUSE_DATABASE}_05485_cas_pool/')"

Q1="${CLICKHOUSE_DATABASE}_05485_insert"
Q2="${CLICKHOUSE_DATABASE}_05485_optimize"

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = $DISK"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t SELECT number FROM numbers(10)"
${CLICKHOUSE_CLIENT} --query_id "$Q1" -q "INSERT INTO t SELECT number FROM numbers(10)"
${CLICKHOUSE_CLIENT} --query_id "$Q2" -q "OPTIMIZE TABLE t FINAL"
${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log"

for q in "$Q1" "$Q2"
do
    ${CLICKHOUSE_CLIENT} -q "
        SELECT ProfileEvents['CASRootOverwrite'] + ProfileEvents['CASRootCompareSwapConflict'],
               ProfileEvents['CASRootGet'],
               ProfileEvents['CASRootPut'],
               ProfileEvents['CASOtherGet']
        FROM system.query_log
        WHERE current_database = currentDatabase() AND query_id = '$q' AND type = 'QueryFinish'"
done

${CLICKHOUSE_CLIENT} -q "DROP TABLE t"

# FORGET logs an operator WARNING; the harness runs the client at --send_logs_level=warning, which would
# stream that expected warning to stderr and be flagged as a failure. Suppress it for the FORGET call only.
${CLICKHOUSE_CLIENT} --send_logs_level=fatal -q "SYSTEM CAS FORGET '${DISK_NAME}'"
