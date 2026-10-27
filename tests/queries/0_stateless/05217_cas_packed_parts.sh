#!/usr/bin/env bash
# Tags: no-fasttest, no-random-merge-tree-settings, no-parallel, cas-storage
# cas-storage: the oracles below read system.cas_log of the default disk and use the cas_packed_mixed policy of the CAS lanes.

set -euo pipefail

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISABLED_DISK_NAME="${CLICKHOUSE_DATABASE}_05217_disabled_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
DISABLED_POOL="${CLICKHOUSE_USER_FILES_UNIQUE}_05217_disabled_${RANDOM}"
COMPACT_QUERY_ID="${CLICKHOUSE_DATABASE}_05217_compact_${CLICKHOUSE_TEST_UNIQUE_NAME}"
WIDE_QUERY_ID="${CLICKHOUSE_DATABASE}_05217_wide_${CLICKHOUSE_TEST_UNIQUE_NAME}"
COMPACT_SALT="${CLICKHOUSE_DATABASE}_${CLICKHOUSE_TEST_UNIQUE_NAME}_compact"
WIDE_SALT="${CLICKHOUSE_DATABASE}_${CLICKHOUSE_TEST_UNIQUE_NAME}_wide"
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

check_table()
{
    local table_name=$1
    local output_name=$2

    ${CLICKHOUSE_CLIENT} --query "CHECK TABLE ${table_name} SETTINGS check_query_single_value_result = 1" \
        | awk -v name="${output_name}" '{ print name, $0 }'
}

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS cas_packed_copy SYNC;
DROP TABLE IF EXISTS cas_packed_destination SYNC;
DROP TABLE IF EXISTS cas_packed_local SYNC;
DROP TABLE IF EXISTS cas_packed_wide SYNC;
DROP TABLE IF EXISTS cas_packed_disabled SYNC;

CREATE TABLE cas_packed_local
(
    id UInt64,
    value UInt64,
    salt UInt64,
    INDEX value_idx value TYPE minmax GRANULARITY 1,
    PROJECTION p1 (SELECT salt, value, count(), sum(id) GROUP BY salt, value),
    PROJECTION p2 (SELECT salt, count(), sum(value) GROUP BY salt)
)
ENGINE = MergeTree
PARTITION BY tuple()
ORDER BY id
SETTINGS min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = '1G',
    index_granularity = 64,
    index_granularity_bytes = 10485760;

CREATE TABLE cas_packed_wide
(
    id UInt64,
    value UInt64,
    salt UInt64,
    INDEX value_idx value TYPE minmax GRANULARITY 1,
    PROJECTION p1 (SELECT salt, value, count(), sum(id) GROUP BY salt, value),
    PROJECTION p2 (SELECT salt, count(), sum(value) GROUP BY salt)
)
ENGINE = MergeTree
PARTITION BY tuple()
ORDER BY id
SETTINGS min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = 0,
    index_granularity = 64,
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
SYSTEM STOP MERGES cas_packed_wide;
EOF

${CLICKHOUSE_CLIENT} --query_id "${COMPACT_QUERY_ID}" \
    --query "INSERT INTO cas_packed_local SELECT number, number % 1000, cityHash64('${COMPACT_SALT}') FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query_id "${WIDE_QUERY_ID}" \
    --query "INSERT INTO cas_packed_wide SELECT number, number % 1000, cityHash64('${WIDE_SALT}') FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_local SELECT number + 10000, number % 1000, cityHash64('${COMPACT_SALT}') FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_local SELECT number + 20000, number % 1000, cityHash64('${COMPACT_SALT}') FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_local SELECT number + 30000, number % 1000, cityHash64('${COMPACT_SALT}') FROM numbers(10000)"
${CLICKHOUSE_CLIENT} --query "INSERT INTO cas_packed_disabled VALUES (1, 1)"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT table, arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase()
    AND table IN ('cas_packed_local', 'cas_packed_wide', 'cas_packed_disabled')
    AND active
GROUP BY table
ORDER BY table;

SELECT 'compact_initial', any(part_type), count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;
SELECT 'wide_initial', any(part_type), count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_wide' AND active;

SELECT 'compact_aggregate', count(), sum(id), sum(value) FROM cas_packed_local;
SELECT 'wide_aggregate', count(), sum(id), sum(value) FROM cas_packed_wide;
SELECT 'compact_row', id, value FROM cas_packed_local WHERE id IN (0, 39999) ORDER BY id;
SELECT 'wide_row', id, value FROM cas_packed_wide WHERE id IN (0, 9999) ORDER BY id;
SELECT 'compact_minmax', count() FROM cas_packed_local WHERE value BETWEEN 998 AND 999
SETTINGS force_data_skipping_indices = 'value_idx', use_skip_indexes_on_data_read = 1, optimize_use_projections = 0;
SELECT 'wide_minmax', count() FROM cas_packed_wide WHERE value BETWEEN 998 AND 999
SETTINGS force_data_skipping_indices = 'value_idx', use_skip_indexes_on_data_read = 1, optimize_use_projections = 0;

SELECT 'compact_projection_parts', min(projection_count), max(projection_count)
FROM
(
    SELECT parent_name, countDistinct(name) AS projection_count
    FROM system.projection_parts
    WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active
    GROUP BY parent_name
);
SELECT 'wide_projection_parts', min(projection_count), max(projection_count)
FROM
(
    SELECT parent_name, countDistinct(name) AS projection_count
    FROM system.projection_parts
    WHERE database = currentDatabase() AND table = 'cas_packed_wide' AND active
    GROUP BY parent_name
);
EOF

check_table cas_packed_local compact_check_insert
check_table cas_packed_wide wide_check_insert

MAIN_DISK=$(${CLICKHOUSE_CLIENT} --query "
    SELECT any(disk_name)
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active")
MAIN_DISK_IS_CAS=$(${CLICKHOUSE_CLIENT} --query "SELECT count() FROM system.cas_mounts WHERE disk = '${MAIN_DISK}'")
if [ "${MAIN_DISK_IS_CAS}" != "1" ]; then
    echo "the default disk '${MAIN_DISK}' is not content-addressed; this test runs on the CAS lanes only" >&2
    exit 1
fi

${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS cas_log"
${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT 'compact_blob_put', count()
FROM system.cas_log
WHERE disk_name = '${MAIN_DISK}' AND event_type = 'blob_put' AND query_id = '${COMPACT_QUERY_ID}';
SELECT 'wide_blob_put', count()
FROM system.cas_log
WHERE disk_name = '${MAIN_DISK}' AND event_type = 'blob_put' AND query_id = '${WIDE_QUERY_ID}';
EOF

${CLICKHOUSE_CLIENT} --multiquery <<EOF

SYSTEM START MERGES cas_packed_local;
SYSTEM START MERGES cas_packed_wide;
OPTIMIZE TABLE cas_packed_local FINAL;
OPTIMIZE TABLE cas_packed_wide FINAL;

SELECT 'compact_merged', count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;
SELECT 'wide_merged', count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_wide' AND active;

ALTER TABLE cas_packed_local MATERIALIZE INDEX value_idx SETTINGS mutations_sync = 2;
ALTER TABLE cas_packed_local MATERIALIZE PROJECTION p1 SETTINGS mutations_sync = 2;
ALTER TABLE cas_packed_local MATERIALIZE PROJECTION p2 SETTINGS mutations_sync = 2;
ALTER TABLE cas_packed_wide MATERIALIZE INDEX value_idx SETTINGS mutations_sync = 2;
ALTER TABLE cas_packed_wide MATERIALIZE PROJECTION p1 SETTINGS mutations_sync = 2;
ALTER TABLE cas_packed_wide MATERIALIZE PROJECTION p2 SETTINGS mutations_sync = 2;

SELECT 'compact_projection_active', countDistinct(name)
FROM system.projection_parts
WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;
SELECT 'wide_projection_active', countDistinct(name)
FROM system.projection_parts
WHERE database = currentDatabase() AND table = 'cas_packed_wide' AND active;

SET optimize_use_projections = 1, force_optimize_projection = 1;
SELECT 'compact_projection_query', value, count(), sum(id)
FROM cas_packed_local
WHERE salt = cityHash64('${COMPACT_SALT}')
GROUP BY value
ORDER BY value
LIMIT 3;
SELECT 'wide_projection_query', value, count(), sum(id)
FROM cas_packed_wide
WHERE salt = cityHash64('${WIDE_SALT}')
GROUP BY value
ORDER BY value
LIMIT 3;
SET force_optimize_projection = 0;

ALTER TABLE cas_packed_local UPDATE value = value + 1 WHERE id = 0 SETTINGS mutations_sync = 2;
SELECT 'compact_after_update', count(), sum(id), sum(value) FROM cas_packed_local;
SELECT 'compact_update_storage', any(part_storage_type)
FROM system.parts WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;
ALTER TABLE cas_packed_local DELETE WHERE id = 1 SETTINGS mutations_sync = 2;
SELECT 'compact_after_delete', count(), sum(id), sum(value) FROM cas_packed_local;
SELECT 'compact_delete_storage', any(part_storage_type)
FROM system.parts WHERE database = currentDatabase() AND table = 'cas_packed_local' AND active;

ALTER TABLE cas_packed_wide UPDATE value = value + 1 WHERE id = 0 SETTINGS mutations_sync = 2;
SELECT 'wide_after_update', count(), sum(id), sum(value) FROM cas_packed_wide;
SELECT 'wide_update_storage', any(part_storage_type)
FROM system.parts WHERE database = currentDatabase() AND table = 'cas_packed_wide' AND active;
ALTER TABLE cas_packed_wide DELETE WHERE id = 1 SETTINGS mutations_sync = 2;
SELECT 'wide_after_delete', count(), sum(id), sum(value) FROM cas_packed_wide;
SELECT 'wide_delete_storage', any(part_storage_type)
FROM system.parts WHERE database = currentDatabase() AND table = 'cas_packed_wide' AND active;

ALTER TABLE cas_packed_local FREEZE WITH NAME '${CLICKHOUSE_DATABASE}_05217';
EOF

${CLICKHOUSE_CLIENT} --query "SYSTEM UNFREEZE WITH NAME '${CLICKHOUSE_DATABASE}_05217'" \
    | ${CLICKHOUSE_LOCAL} --structure "${UNFREEZE_STRUCTURE}" \
        --query "SELECT 'unfreeze_parts', count() FROM table"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
ALTER TABLE cas_packed_local DETACH PARTITION tuple();
ALTER TABLE cas_packed_local ATTACH PARTITION tuple();
SELECT 'compact_partition_reload', count(), sum(id), sum(value) FROM cas_packed_local;
ALTER TABLE cas_packed_wide DETACH PARTITION tuple();
ALTER TABLE cas_packed_wide ATTACH PARTITION tuple();
SELECT 'wide_partition_reload', count(), sum(id), sum(value) FROM cas_packed_wide;
EOF

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DETACH TABLE cas_packed_local;
ATTACH TABLE cas_packed_local;
SELECT 'compact_table_reload', count(), sum(id), sum(value) FROM cas_packed_local;
DETACH TABLE cas_packed_wide;
ATTACH TABLE cas_packed_wide;
SELECT 'wide_table_reload', count(), sum(id), sum(value) FROM cas_packed_wide;
EOF

check_table cas_packed_local compact_check_attach
check_table cas_packed_wide wide_check_attach

${CLICKHOUSE_CLIENT} --multiquery <<EOF
CREATE TABLE cas_packed_copy
(
    id UInt64,
    value UInt64,
    salt UInt64,
    INDEX value_idx value TYPE minmax GRANULARITY 1,
    PROJECTION p1 (SELECT salt, value, count(), sum(id) GROUP BY salt, value),
    PROJECTION p2 (SELECT salt, count(), sum(value) GROUP BY salt)
)
ENGINE = MergeTree
PARTITION BY tuple()
ORDER BY id
SETTINGS min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = '1G',
    index_granularity = 64,
    index_granularity_bytes = 10485760;

ALTER TABLE cas_packed_copy ATTACH PARTITION tuple() FROM cas_packed_local;
SELECT 'copy_rows', count(), sum(id), sum(value) FROM cas_packed_copy;
SELECT 'copy_storage', arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ',')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_copy' AND active;
SELECT 'copy_projection_active', countDistinct(name)
FROM system.projection_parts
WHERE database = currentDatabase() AND table = 'cas_packed_copy' AND active;
SELECT 'copy_projection_parts', min(projection_count), max(projection_count)
FROM
(
    SELECT parent_name, countDistinct(name) AS projection_count
    FROM system.projection_parts
    WHERE database = currentDatabase() AND table = 'cas_packed_copy' AND active
    GROUP BY parent_name
);
EOF

check_table cas_packed_copy copy_check_attach

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE cas_packed_local SYNC;
SELECT 'copy_after_source_drop', count(), sum(id), sum(value) FROM cas_packed_copy;
DETACH TABLE cas_packed_copy;
ATTACH TABLE cas_packed_copy;
SELECT 'copy_after_reload', count(), sum(id), sum(value) FROM cas_packed_copy;
EOF

HAS_MIXED_POLICY=$(${CLICKHOUSE_CLIENT} --query "
    SELECT countDistinct(policy_name)
    FROM system.storage_policies
    WHERE policy_name = 'cas_packed_mixed'")
if [ "${HAS_MIXED_POLICY}" != "1" ]; then
    echo "storage policy 'cas_packed_mixed' is missing; the CAS lane config defines it" >&2
    exit 1
fi

${CLICKHOUSE_CLIENT} --multiquery <<EOF
CREATE TABLE cas_packed_destination (id UInt64, value UInt64)
ENGINE = MergeTree
ORDER BY id
SETTINGS storage_policy = 'cas_packed_mixed',
min_bytes_for_full_part_storage = '1G';

INSERT INTO cas_packed_destination VALUES (1, 10), (2, 20);

SELECT 'destination_initial', any(part_storage_type), countIf(disk_name = 'cas_packed_disabled')
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active;
EOF

DESTINATION_PART=$(${CLICKHOUSE_CLIENT} --query "
    SELECT name
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active")
${CLICKHOUSE_CLIENT} --query "ALTER TABLE cas_packed_destination MOVE PART '${DESTINATION_PART}' TO VOLUME 'full'"
${CLICKHOUSE_CLIENT} --query "
    SELECT 'destination_moved_disabled', any(part_storage_type), countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active"

${CLICKHOUSE_CLIENT} --query "
    ALTER TABLE cas_packed_destination UPDATE value = value + 1 WHERE id = 1
    SETTINGS mutations_sync = 2"
${CLICKHOUSE_CLIENT} --query "
    SELECT 'destination_mutated_disabled', any(part_storage_type), countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active"

DESTINATION_PART=$(${CLICKHOUSE_CLIENT} --query "
    SELECT name
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active")
${CLICKHOUSE_CLIENT} --query "ALTER TABLE cas_packed_destination MOVE PART '${DESTINATION_PART}' TO VOLUME 'packed'"
${CLICKHOUSE_CLIENT} --query "
    SELECT 'destination_moved_enabled', any(part_storage_type), countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active"

${CLICKHOUSE_CLIENT} --query "
    ALTER TABLE cas_packed_destination UPDATE value = value + 1 WHERE id = 2
    SETTINGS mutations_sync = 2"
${CLICKHOUSE_CLIENT} --query "
    SELECT 'destination_mutated_enabled', any(part_storage_type), countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active"
${CLICKHOUSE_CLIENT} --query "
    OPTIMIZE TABLE cas_packed_destination FINAL
    SETTINGS optimize_throw_if_noop = 1"
${CLICKHOUSE_CLIENT} --query "
    SELECT 'destination_merged_enabled', any(part_storage_type), countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = 'cas_packed_destination' AND active"
${CLICKHOUSE_CLIENT} --query "DROP TABLE cas_packed_destination SYNC"

${CLICKHOUSE_CLIENT} --query "DROP TABLE cas_packed_copy SYNC"
${CLICKHOUSE_CLIENT} --query "DROP TABLE cas_packed_wide SYNC"
${CLICKHOUSE_CLIENT} --query "DROP TABLE cas_packed_disabled SYNC"

drain_gc "${MAIN_DISK}" default
check_fsck "${MAIN_DISK}" default

drain_gc cas_packed_disabled destination
check_fsck cas_packed_disabled destination

drain_gc "${DISABLED_DISK_NAME}" disabled
check_fsck "${DISABLED_DISK_NAME}" disabled

${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISABLED_DISK_NAME}'"

rm -rf "${DISABLED_POOL:?}"
