#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# Removing an outdated part from a cas disk must be one ref drop: no `delete_tmp_` ref, no repoint of a
# renamed ref, no manifest publish under a ref other than the parts' own. The oracle is `system.cas_log`,
# selected by what the events name (the table's namespace and the part ref names the test knows), not by
# time: audit events are timestamped at delivery and can arrive after the removal returns. The test
# waits for the terminal records, one `ref_drop` per outdated part, before counting. Background GC is off
# on the disk. The namespace filter drops the disk's mount and watermark events and needs a table with a
# UUID (an `Atomic` or `Replicated` database).

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="${CLICKHOUSE_DATABASE}_05054_cas"
DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05054',
    cas_gc_enabled = 0,
    name = '${DISK_NAME}',
    path = '${CLICKHOUSE_DATABASE}_05054_cas_pool/')"

run_case()
{
    local name=$1
    local projection=$2

    ${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_$name"
    ${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_$name (a UInt64, b UInt64 $projection) ENGINE = MergeTree ORDER BY a
        SETTINGS disk = $DISK, old_parts_lifetime = 0, cleanup_delay_period = 1, max_cleanup_delay_period = 1, cleanup_delay_period_random_add = 0"
    ${CLICKHOUSE_CLIENT} -q "SYSTEM STOP CLEANUP t_$name"
    for i in 1 2 3 4
    do
        ${CLICKHOUSE_CLIENT} -q "INSERT INTO t_$name SELECT number, $i FROM numbers(100)"
    done
    ${CLICKHOUSE_CLIENT} -q "OPTIMIZE TABLE t_$name FINAL"

    ${CLICKHOUSE_CLIENT} -q "SELECT '$name', 'outdated_parts_present', count() > 0 FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$name' AND NOT active"
    local outdated_names all_names active_name table_uuid
    outdated_names=$(${CLICKHOUSE_CLIENT} -q "SELECT groupArray(name) FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$name' AND NOT active")
    all_names=$(${CLICKHOUSE_CLIENT} -q "SELECT groupArray(name) FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$name'")
    active_name=$(${CLICKHOUSE_CLIENT} -q "SELECT name FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$name' AND active")
    table_uuid=$(${CLICKHOUSE_CLIENT} -q "SELECT uuid FROM system.tables
        WHERE database = currentDatabase() AND name = 't_$name'")
    local outdated
    outdated=$(${CLICKHOUSE_CLIENT} -q "SELECT length($outdated_names)")

    ${CLICKHOUSE_CLIENT} -q "SYSTEM START CLEANUP t_$name"
    local left
    for _ in {1..120}
    do
        left=$(${CLICKHOUSE_CLIENT} -q "SELECT count() FROM system.parts
            WHERE database = currentDatabase() AND table = 't_$name' AND NOT active")
        [ "$left" = "0" ] && break
        sleep 0.5
    done
    ${CLICKHOUSE_CLIENT} -q "SELECT '$name', 'outdated_parts_left', count() FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$name' AND NOT active"

    # Audit events are delivered asynchronously: wait until the terminal record of every removal is there.
    local drops
    for _ in {1..120}
    do
        ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
        drops=$(${CLICKHOUSE_CLIENT} -q "SELECT countIf(event_type = 'ref_drop' AND has($outdated_names, ref_name))
            FROM system.cas_log WHERE disk_name = '${DISK_NAME}' AND position(namespace, '$table_uuid') > 0")
        [ "$drops" -ge "$outdated" ] && break
        sleep 0.5
    done

    ${CLICKHOUSE_CLIENT} -q "
        SELECT
            '$name',
            'delete_tmp_events', countIf(position(ref_name, 'delete_tmp_') > 0),
            'ref_repoint', countIf(event_type = 'ref_repoint'),
            'extra_manifest_publish', countIf(event_type = 'build_publish' AND NOT has($all_names, ref_name)),
            'ref_drop_per_part', countIf(event_type = 'ref_drop' AND has($outdated_names, ref_name)) / $outdated,
            'active_part_ref_drops', countIf(event_type = 'ref_drop' AND ref_name = '$active_name')
        FROM system.cas_log
        WHERE disk_name = '${DISK_NAME}' AND position(namespace, '$table_uuid') > 0"

    ${CLICKHOUSE_CLIENT} -q "SELECT '$name', 'rows', count(), sum(b) FROM t_$name"

    ${CLICKHOUSE_CLIENT} -q "DROP TABLE t_$name"
}

run_case plain ""
run_case projection ", PROJECTION p (SELECT a, sum(b) GROUP BY a)"

# FORGET logs an operator WARNING; the harness runs the client at --send_logs_level=warning, which would
# stream that expected warning to stderr and be flagged as a failure. Suppress it for the FORGET call only.
${CLICKHOUSE_CLIENT} --send_logs_level=fatal -q "SYSTEM CAS FORGET '${DISK_NAME}'"
