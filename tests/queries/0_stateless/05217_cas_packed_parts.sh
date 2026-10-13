#!/usr/bin/env bash
# Tags: no-fasttest, no-random-merge-tree-settings, no-parallel

set -euo pipefail

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISABLED_DISK_NAME="${CLICKHOUSE_DATABASE}_05217_disabled_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
DISABLED_POOL="${CLICKHOUSE_USER_FILES_UNIQUE}_05217_disabled_${RANDOM}"
UNFREEZE_STRUCTURE='command_type String, partition_id String, part_name String, backup_name String, backup_path String, part_backup_path String'

rm -rf "${DISABLED_POOL:?}"
mkdir -p "${DISABLED_POOL}"

drain_gc()
{
    local disk_name=$1
    local output_name=$2
    local pending=1

    for _ in $(seq 1 60); do
        pending=$(${CLICKHOUSE_CLIENT} --query "SYSTEM CAS GC RUN '${disk_name}'" --format TSVWithNames \
            | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next } { print $col["pending_condemned"] }')
        [ "${pending}" = "0" ] && break
        sleep 0.5
    done

    if [ "${pending}" != "0" ]; then
        echo "GC did not drain for ${disk_name}: pending=${pending}" >&2
        exit 1
    fi

    echo "${output_name}_gc_drained" 1
}

check_fsck()
{
    local disk_name=$1
    local output_name=$2

    ${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK '${disk_name}'" --format TSVWithNames \
        | awk -F'\t' -v name="${output_name}" 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
            { print name "_fsck_unreachable", $col["unreachable"]; print name "_fsck_dangling", $col["dangling"] }'
}

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS cas_packed_copy SYNC;
DROP TABLE IF EXISTS cas_packed_local SYNC;
DROP TABLE IF EXISTS cas_packed_disabled SYNC;

CREATE TABLE cas_packed_local
(
    id UInt64,
    value UInt64,
    INDEX value_idx value TYPE minmax GRANULARITY 1,
    PROJECTION value_projection (SELECT value, count(), sum(id) GROUP BY value)
)
ENGINE = MergeTree
PARTITION BY tuple()
ORDER BY id
SETTINGS min_bytes_for_full_part_storage = '1G',
    index_granularity = 8192,
    index_granularity_bytes = 10485760;

CREATE TABLE cas_packed_disabled (id UInt64, value UInt64)
ENGINE = MergeTree
ORDER BY id
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05217_disabled',
    name = '${DISABLED_DISK_NAME}',
    path = '${DISABLED_POOL}/',
    cas_gc_enabled = 1,
    cas_gc_interval_sec = 1),
    min_bytes_for_full_part_storage = '1G';

SYSTEM STOP MERGES cas_packed_local;
EOF

${CLICKHOUSE_CLIENT} --query_id "${CLICKHOUSE_DATABASE}_05217_blob_put" \
    --query "INSERT INTO cas_packed_local SELECT number, number % 1000 FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_local SELECT number + 10000, number % 1000 FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_local SELECT number + 20000, number % 1000 FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_local SELECT number + 30000, number % 1000 FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_disabled VALUES (1, 1)"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT table, arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase()
    AND table IN ('cas_packed_local', 'cas_packed_disabled')
    AND active
GROUP BY table
ORDER BY table;

SELECT 'initial_parts', count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;

SELECT 'aggregate', count(), sum(id), sum(value) FROM cas_packed_local;
SELECT 'minmax_query', count() FROM cas_packed_local WHERE value BETWEEN 998 AND 999;

SYSTEM START MERGES cas_packed_local;
OPTIMIZE TABLE cas_packed_local FINAL;

SELECT 'merged_parts', count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;

ALTER TABLE cas_packed_local MATERIALIZE INDEX value_idx SETTINGS mutations_sync = 2;
ALTER TABLE cas_packed_local MATERIALIZE PROJECTION value_projection SETTINGS mutations_sync = 2;

SELECT 'projection_active', countDistinct(name)
FROM system.projection_parts
WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;

SET optimize_use_projections = 1, force_optimize_projection = 1;
SELECT 'projection_query', value, count(), sum(id)
FROM cas_packed_local
GROUP BY value
ORDER BY value
LIMIT 3;
SET force_optimize_projection = 0;

CHECK TABLE cas_packed_local SETTINGS check_query_single_value_result = 1;
ALTER TABLE cas_packed_local FREEZE WITH NAME '${CLICKHOUSE_DATABASE}_05217';
EOF

${CLICKHOUSE_CLIENT} --query "SYSTEM UNFREEZE WITH NAME '${CLICKHOUSE_DATABASE}_05217'" \
    | ${CLICKHOUSE_LOCAL} --structure "${UNFREEZE_STRUCTURE}" \
        --query "SELECT 'unfreeze_parts', count() FROM table"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
ALTER TABLE cas_packed_local DETACH PARTITION tuple();
ALTER TABLE cas_packed_local ATTACH PARTITION tuple();
SELECT 'partition_reload', count(), sum(id) FROM cas_packed_local;

DETACH TABLE cas_packed_local;
ATTACH TABLE cas_packed_local;
SELECT 'table_reload', count(), sum(id) FROM cas_packed_local;

CREATE TABLE cas_packed_copy
(
    id UInt64,
    value UInt64,
    INDEX value_idx value TYPE minmax GRANULARITY 1,
    PROJECTION value_projection (SELECT value, count(), sum(id) GROUP BY value)
)
ENGINE = MergeTree
PARTITION BY tuple()
ORDER BY id
SETTINGS min_bytes_for_full_part_storage = '1G',
    index_granularity = 8192,
    index_granularity_bytes = 10485760;

ALTER TABLE cas_packed_copy ATTACH PARTITION tuple() FROM cas_packed_local;
SELECT 'copy_rows', count(), sum(id) FROM cas_packed_copy;
SELECT 'copy_storage', arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_copy' AND active;
SELECT 'copy_projection_active', countDistinct(name)
FROM system.projection_parts
WHERE database = currentDatabase() AND table = 'cas_packed_copy' AND active;

DROP TABLE cas_packed_local SYNC;
SELECT 'copy_after_source_drop', count(), sum(id) FROM cas_packed_copy;
DETACH TABLE cas_packed_copy;
ATTACH TABLE cas_packed_copy;
SELECT 'copy_after_reload', count(), sum(id) FROM cas_packed_copy;
EOF

MAIN_DISK=$(${CLICKHOUSE_CLIENT} --query "
    SELECT any(disk_name)
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_copy' AND active")
MAIN_DISK_IS_CAS=$(${CLICKHOUSE_CLIENT} --query "SELECT count() FROM system.cas_mounts WHERE disk = '${MAIN_DISK}'")

if [ "${MAIN_DISK_IS_CAS}" = "1" ]; then
    ${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS cas_log"
    ${CLICKHOUSE_CLIENT} --query "
        SELECT 'has_blob_put', count() > 0
        FROM system.cas_log
        WHERE disk_name = '${MAIN_DISK}'
            AND event_type = 'blob_put'
            AND query_id = '${CLICKHOUSE_DATABASE}_05217_blob_put'"
else
    printf 'has_blob_put\t1\n'
fi

${CLICKHOUSE_CLIENT} --query "DROP TABLE cas_packed_copy SYNC"
${CLICKHOUSE_CLIENT} --query "DROP TABLE cas_packed_disabled SYNC"

if [ "${MAIN_DISK_IS_CAS}" = "1" ]; then
    drain_gc "${MAIN_DISK}" default
    check_fsck "${MAIN_DISK}" default
else
    echo "default_gc_drained" 1
    echo "default_fsck_unreachable" 0
    echo "default_fsck_dangling" 0
fi

drain_gc "${DISABLED_DISK_NAME}" disabled
check_fsck "${DISABLED_DISK_NAME}" disabled

${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISABLED_DISK_NAME}'"

rm -rf "${DISABLED_POOL:?}"
