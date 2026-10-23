#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# A deduplicated INSERT on a content-addressed disk: the rejected part was already committed under its
# temporary name (the storage transaction commits before the parts lock), so its ref must be dropped
# again. Every committed `tmp_*` ref is either moved to a final name or dropped; none stays.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

D="${CLICKHOUSE_DATABASE}_dd1"
DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${D}', name = '${D}', path = '${CLICKHOUSE_USER_FILES_UNIQUE}_dd1/', cas_gc_enabled = 0)"

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_dd1 SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_dd1 (a UInt64, v UInt64) ENGINE = MergeTree ORDER BY a
    SETTINGS disk = ${DISK}, non_replicated_deduplication_window = 100, min_bytes_for_full_part_storage = 0"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_dd1"
# `INSERT SELECT` deduplicates only with an explicit token (its block boundaries are not deterministic).
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_dd1 SELECT number, number FROM numbers(1000) SETTINGS insert_deduplicate = 1, insert_deduplication_token = 'dd1'"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_dd1 SELECT number, number FROM numbers(1000) SETTINGS insert_deduplicate = 1, insert_deduplication_token = 'dd1'"
${CLICKHOUSE_CLIENT} -q "SELECT 'rows', count() FROM t_dd1"
${CLICKHOUSE_CLIENT} -q "SELECT 'active_parts', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_dd1' AND active"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
# Published under tmp_*: two (both inserts committed a part); moved: one; dropped: one; nothing left.
${CLICKHOUSE_CLIENT} -q "SELECT 'tmp_refs',
        countIf(event_type = 'build_publish' AND startsWith(ref_name, 'tmp_')) AS published,
        countIf(event_type = 'ref_move' AND startsWith(ref_name, 'tmp_')) AS moved,
        countIf(event_type = 'ref_drop' AND startsWith(ref_name, 'tmp_')) AS dropped,
        published - moved - dropped AS left_behind
    FROM system.cas_log WHERE disk_name = '${D}'"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_dd1 SYNC"
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal -q "SYSTEM CAS FORGET '${D}'"
rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE:?}_dd1"
