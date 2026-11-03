#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# A committed part rename on a content-addressed disk is one owner move (ref_move), not a second
# manifest. ref_drop is not counted: outdated parts are removed by the cleanup thread on its own
# schedule.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_pc2', name = '${CLICKHOUSE_DATABASE}_pc2', path = '${CLICKHOUSE_DATABASE}_pc2_pool/')"

# Prints the event counts accumulated since the previous call (nothing for an empty label), so the
# writers that fill the source table do not affect the numbers of the operation under test.
PREV="0 0 0"
counts() {
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    CUR=$(${CLICKHOUSE_CLIENT} -q "SELECT countIf(event_type = 'build_publish'), countIf(event_type = 'manifest_put'), countIf(event_type = 'ref_move')
        FROM system.cas_log WHERE disk_name = '${CLICKHOUSE_DATABASE}_pc2'")
    read -r p0 m0 r0 <<< "$PREV"
    read -r p1 m1 r1 <<< "$CUR"
    if [ -n "$1" ]; then echo "$1 build_publish $((p1 - p0)) manifest_put $((m1 - m0)) ref_move $((r1 - r0))"; fi
    PREV="$CUR"
}

for t in t_src t_dst
do
    ${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS $t SYNC"
    ${CLICKHOUSE_CLIENT} -q "CREATE TABLE $t (a UInt64, v UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0, enable_block_number_column = 1, enable_block_offset_column = 1"
done
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_dst"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_src SELECT number, number FROM numbers(1000)"
${CLICKHOUSE_CLIENT} --enable_lightweight_update 1 -q "UPDATE t_src SET v = v + 1 WHERE a % 100 = 0"
counts ""
# ATTACH FROM and MOVE TO TABLE refuse a partition with unapplied patch parts.
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_src APPLY PATCHES SETTINGS mutations_sync = 2"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_src"
counts ""
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_dst ATTACH PARTITION tuple() FROM t_src"
# One part moves its patch-free copy in: one publish of the attached part and one owner move.
counts attach_from
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_src MOVE PARTITION tuple() TO TABLE t_dst"
# The destination gets a clone published as a new part and moved into place; the source gets an empty
# covering part, published without a move.
counts move_to_table
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_src SYNC"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_dst SYNC"
