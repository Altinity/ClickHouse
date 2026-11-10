#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# TRUNCATE on a content-addressed disk publishes the empty part once, under its temporary name and off the
# parts lock, and renames it with one owner move.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_tr', name = '${CLICKHOUSE_DATABASE}_tr', path = '${CLICKHOUSE_DATABASE}_tr_pool/')"

# Prints the event counts accumulated since the previous call (nothing for an empty label).
PREV="0 0 0"
counts() {
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    CUR=$(${CLICKHOUSE_CLIENT} -q "SELECT countIf(event_type = 'build_publish'), countIf(event_type = 'manifest_put'), countIf(event_type = 'ref_move')
        FROM system.cas_log WHERE disk_name = '${CLICKHOUSE_DATABASE}_tr'")
    read -r p0 m0 r0 <<< "$PREV"
    read -r p1 m1 r1 <<< "$CUR"
    if [ -n "$1" ]; then echo "$1 build_publish $((p1 - p0)) manifest_put $((m1 - m0)) ref_move $((r1 - r0))"; fi
    PREV="$CUR"
}

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_tr SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_tr (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_tr SELECT number FROM numbers(10)"
counts ""
${CLICKHOUSE_CLIENT} -q "TRUNCATE TABLE t_tr"
counts truncate
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM t_tr"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_tr SYNC"
