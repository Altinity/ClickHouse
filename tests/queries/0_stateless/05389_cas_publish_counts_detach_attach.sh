#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# Renames inside detached/ on a content-addressed disk are owner moves, not republications: ATTACH renames
# the detached part to `detached/attaching_<part>` and then to its live name, each as one `ref_move`.
# DETACH itself clones the part (a new manifest) and is not a rename. `min_bytes_for_full_part_storage = 0`
# pins off packed parts: packed parts on a content-addressed disk are a known separate defect.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_pc2', name = '${CLICKHOUSE_DATABASE}_pc2', path = '${CLICKHOUSE_DATABASE}_pc2_pool/')"

# Both ATTACH renames are `ref_move`s whose source is under detached/.
counts() {
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    ${CLICKHOUSE_CLIENT} -q "SELECT '$1',
            countIf(event_type = 'ref_move' AND startsWith(ref_name, 'detached/')) AS detached_moves
         FROM system.cas_log
         WHERE disk_name = '${CLICKHOUSE_DATABASE}_pc2'"
}

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_pc2 SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_pc2 (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_pc2"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_pc2 SELECT number FROM numbers(1000)"
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_pc2 DETACH PARTITION tuple()"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM t_pc2"
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_pc2 ATTACH PARTITION tuple()"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM t_pc2"
counts detach_attach
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_pc2 SYNC"
