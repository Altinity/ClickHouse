#!/usr/bin/env bash
# Tags: no-fasttest, no-random-merge-tree-settings, cas-storage
# cas-storage: uses the cas_packed_mixed policy of the CAS lane config.

set -euo pipefail

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$CUR_DIR"/../shell_config.sh

TABLE_NAME="cas_packed_merge_disabled_destination"

HAS_MIXED_POLICY=$(${CLICKHOUSE_CLIENT} --query "
    SELECT countDistinct(policy_name)
    FROM system.storage_policies
    WHERE policy_name = 'cas_packed_mixed'")

if [ "${HAS_MIXED_POLICY}" != "1" ]; then
    echo "storage policy 'cas_packed_mixed' is missing; the CAS lane config defines it" >&2
    exit 1
fi

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS ${TABLE_NAME} SYNC;

CREATE TABLE ${TABLE_NAME} (id UInt64, value UInt64)
ENGINE = MergeTree
ORDER BY id
SETTINGS storage_policy = 'cas_packed_mixed',
    min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = 0,
    enable_vertical_merge_algorithm = 1,
    vertical_merge_algorithm_min_rows_to_activate = 1,
    vertical_merge_algorithm_min_bytes_to_activate = 0,
    vertical_merge_algorithm_min_columns_to_activate = 1;

SYSTEM STOP MERGES ${TABLE_NAME};

INSERT INTO ${TABLE_NAME} VALUES (1, 10);
INSERT INTO ${TABLE_NAME} VALUES (2, 20);
EOF

mapfile -t PART_NAMES < <(${CLICKHOUSE_CLIENT} --query "
    SELECT name
    FROM system.parts
    WHERE database = currentDatabase() AND table = '${TABLE_NAME}' AND active
    ORDER BY name")

for part_name in "${PART_NAMES[@]}"; do
    ${CLICKHOUSE_CLIENT} --query "ALTER TABLE ${TABLE_NAME} MOVE PART '${part_name}' TO VOLUME 'full'"
done

${CLICKHOUSE_CLIENT} --query "
    SELECT 'packed_before_merge', count(), any(part_type), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ','),
        countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = '${TABLE_NAME}' AND active"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SYSTEM START MERGES ${TABLE_NAME};
OPTIMIZE TABLE ${TABLE_NAME} FINAL;
SYSTEM FLUSH LOGS part_log;
EOF

${CLICKHOUSE_CLIENT} --query "
    SELECT 'merge_algorithm', merge_algorithm
    FROM system.part_log
    WHERE database = currentDatabase() AND table = '${TABLE_NAME}' AND event_type = 'MergeParts'
        AND length(merged_from) = 2
    ORDER BY event_time_microseconds DESC
    LIMIT 1"
${CLICKHOUSE_CLIENT} --query "SELECT 'rows_after_merge', count(), sum(id), sum(value) FROM ${TABLE_NAME}"
${CLICKHOUSE_CLIENT} --query "
    SELECT 'storage_after_merge', count(), arrayStringConcat(arraySort(groupUniqArray(part_storage_type)), ','),
        countIf(disk_name = 'cas_packed_disabled')
    FROM system.parts
    WHERE database = currentDatabase() AND table = '${TABLE_NAME}' AND active"

${CLICKHOUSE_CLIENT} --query "DROP TABLE ${TABLE_NAME} SYNC"
