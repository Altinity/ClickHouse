#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# One mutation = one manifest whatever the number of projections: `checkpointTransaction` is a no-op on a
# content-addressed disk, so the per-projection checkpoints of MutateTask publish nothing on their own
# (no partial `tmp_mut_*` manifest, no repoint, no aborted build). Full parts only: packed parts are a
# separate feature.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

D="${CLICKHOUSE_DATABASE}_mp1"
DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${D}', name = '${D}', path = '${CLICKHOUSE_USER_FILES_UNIQUE}_mp1/', cas_gc_enabled = 0)"

# Event totals for the disk so far; a step's cost is the delta.
totals() {
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    ${CLICKHOUSE_CLIENT} -q "SELECT countIf(event_type = 'manifest_put'), countIf(event_type = 'ref_repoint'), countIf(event_type = 'build_abort')
        FROM system.cas_log WHERE disk_name = '${D}'" | tr '\t' ' '
}
step() {
    local label="$1"; shift
    read -r mp0 rp0 ab0 <<< "$(totals)"
    "$@"
    read -r mp1 rp1 ab1 <<< "$(totals)"
    echo "$label manifest_put=$((mp1 - mp0)) ref_repoint=$((rp1 - rp0)) build_abort=$((ab1 - ab0))"
}

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_mp1 SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_mp1 (a UInt64, v UInt64, w UInt64, unused UInt64,
    PROJECTION p1 (SELECT v % 10, sum(a) GROUP BY v % 10),
    PROJECTION p2 (SELECT v % 7, count() GROUP BY v % 7),
    PROJECTION p3 (SELECT a, v ORDER BY v),
    PROJECTION p4 (SELECT v % 3, max(a) GROUP BY v % 3),
    PROJECTION p5 (SELECT v % 5, min(a) GROUP BY v % 5))
    ENGINE = MergeTree ORDER BY a
    SETTINGS disk = ${DISK}, min_bytes_for_full_part_storage = 0, min_bytes_for_wide_part = 0"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_mp1 SELECT number, number, number, number FROM numbers(1000)"

# `w` belongs to no projection: the projections are carried over by hardlink (MutateSomePartColumnsTask).
step update_untouched_projections ${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_mp1 UPDATE w = w + 1 WHERE 1 SETTINGS mutations_sync = 2"
# A column no projection uses goes away: the other mutation branch.
step drop_unused_column ${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_mp1 DROP COLUMN unused SETTINGS mutations_sync = 2"

${CLICKHOUSE_CLIENT} -q "SELECT 'rows', count(), sum(a), sum(v), sum(w) FROM t_mp1"
${CLICKHOUSE_CLIENT} -q "SELECT 'p1', count(), sum(s) FROM (SELECT v % 10 AS k, sum(a) AS s FROM t_mp1 GROUP BY k) SETTINGS force_optimize_projection = 1"
${CLICKHOUSE_CLIENT} -q "SELECT 'p4', count(), sum(m) FROM (SELECT v % 3 AS k, max(a) AS m FROM t_mp1 GROUP BY k) SETTINGS force_optimize_projection = 1"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_mp1 SYNC"
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal -q "SYSTEM CAS FORGET '${D}'"
rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE:?}_mp1"
