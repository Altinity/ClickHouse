#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# An inserted or fetched part is published once under a temporary name and renamed with one owner move.
# The packed part storage is pinned off: fetching a packed part onto a content-addressed disk fails
# with BAD_SIZE_OF_FILE_IN_DATA_PART (count.txt) when the randomized `min_bytes_for_full_part_storage`
# makes the part packed.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_A="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_pc3a', name = '${CLICKHOUSE_DATABASE}_pc3a', path = '${CLICKHOUSE_DATABASE}_pc3a_pool/')"
DISK_B="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_pc3b', name = '${CLICKHOUSE_DATABASE}_pc3b', path = '${CLICKHOUSE_DATABASE}_pc3b_pool/')"

counts() {
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    ${CLICKHOUSE_CLIENT} -q "SELECT '$2', event_type, count() FROM system.cas_log
        WHERE disk_name = '${CLICKHOUSE_DATABASE}_$1'
          AND event_type IN ('build_publish', 'manifest_put', 'ref_move')
        GROUP BY event_type ORDER BY event_type"
}

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS r1 SYNC"
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS r2 SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE r1 (a UInt64, v UInt64) ENGINE = ReplicatedMergeTree('/clickhouse/{database}/pc3', 'r1') ORDER BY a SETTINGS disk = ${DISK_A}, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE r2 (a UInt64, v UInt64) ENGINE = ReplicatedMergeTree('/clickhouse/{database}/pc3', 'r2') ORDER BY a SETTINGS disk = ${DISK_B}, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES r1"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES r2"
${CLICKHOUSE_CLIENT} -q "INSERT INTO r1 SELECT number, number FROM numbers(1000)"
${CLICKHOUSE_CLIENT} -q "SYSTEM SYNC REPLICA r2"
counts pc3a replicated_insert
counts pc3b fetch

# A replicated merge renames its result inside the still-open storage transaction and commits once under
# the final name (MergeFromLogEntryTask::finalize): one manifest, no owner move. The plain MergeTree merge
# commits before the rename and moves (05388).
${CLICKHOUSE_CLIENT} -q "SYSTEM START MERGES r1"
${CLICKHOUSE_CLIENT} -q "INSERT INTO r1 SELECT number, number FROM numbers(1000, 1000)"
${CLICKHOUSE_CLIENT} -q "OPTIMIZE TABLE r1 FINAL"
${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
${CLICKHOUSE_CLIENT} -q "SELECT 'replicated_merge',
        countIf(event_type = 'build_publish' AND ref_name = 'all_0_1_1') AS published_under_final_name,
        countIf(event_type = 'ref_move' AND (ref_name = 'all_0_1_1' OR detail['dst_ref'] = 'all_0_1_1')) AS moves,
        countIf(event_type = 'manifest_put') AS manifests_on_disk
    FROM system.cas_log WHERE disk_name = '${CLICKHOUSE_DATABASE}_pc3a'"
${CLICKHOUSE_CLIENT} -q "DROP TABLE r1 SYNC"
${CLICKHOUSE_CLIENT} -q "DROP TABLE r2 SYNC"
