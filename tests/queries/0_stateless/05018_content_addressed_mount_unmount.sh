#!/usr/bin/env bash
# Tags: no-fasttest
# ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

# Task 6: `SYSTEM CONTENT ADDRESSED UNMOUNT` / `MOUNT` drive the disk lifecycle state machine
# (Task 5) end to end via SQL: the live-table guard refuses `UNMOUNT` while a table still uses the
# disk, both verbs are idempotent, a `Dormant` disk refuses new activity with the `MOUNT` hint, and
# `MOUNT` restores it. Also covers the cross-task fix carried from Task 4's review: a GC round
# dispatched at a `Dormant` disk (now reachable from SQL via `UNMOUNT`) must be a clean
# `INVALID_STATE` refusal, not the `LOGICAL_ERROR` those entry points used to throw (which aborts
# the process under debug/ASan builds).

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="05018_content_addressed_mount_unmount"
DISK_CA="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    server_root_id = '05018',
    name = '${DISK_NAME}',
    path = '05018_content_addressed_mount_unmount_pool/')"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_mount_unmount   SYNC"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_mount_unmount_2 SYNC"

# --- UNMOUNT is refused while a live table references the disk ---
${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_mount_unmount (id UInt64) ENGINE = MergeTree ORDER BY id
SETTINGS disk = ${DISK_CA}"

echo -n 'unmount_refused_live_table: '
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED UNMOUNT '${DISK_NAME}'" 2>&1 \
    | grep -cm1 "live table(s) reference it"

# --- Drop the table, then UNMOUNT succeeds ---
${CLICKHOUSE_CLIENT} --query "DROP TABLE t_mount_unmount SYNC"

${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED UNMOUNT '${DISK_NAME}'"
echo 'unmount_ok'

# --- A second UNMOUNT is an idempotent no-op ---
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED UNMOUNT '${DISK_NAME}'"
echo 'unmount_idempotent_ok'

# --- A Dormant disk refuses new activity, pointing at MOUNT ---
echo -n 'dormant_refuses_new_table: '
${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_mount_unmount_2 (id UInt64) ENGINE = MergeTree ORDER BY id
SETTINGS disk = ${DISK_CA}" 2>&1 | grep -cm1 "is not mounted"

# --- A GC round on the same Dormant disk is a clean INVALID_STATE refusal, never an abort ---
echo -n 'gc_run_on_dormant_disk_clean_error: '
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED GC RUN '${DISK_NAME}'" 2>&1 \
    | grep -cm1 "is not mounted"

# --- MOUNT restores it ---
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED MOUNT '${DISK_NAME}'"
echo 'mount_ok'

# --- A second MOUNT is an idempotent no-op ---
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED MOUNT '${DISK_NAME}'"
echo 'mount_idempotent_ok'

# --- Table creation and use works again after remount ---
${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_mount_unmount_2 (id UInt64) ENGINE = MergeTree ORDER BY id
SETTINGS disk = ${DISK_CA}"
${CLICKHOUSE_CLIENT} --query "INSERT INTO t_mount_unmount_2 VALUES (42)"
${CLICKHOUSE_CLIENT} --query "SELECT * FROM t_mount_unmount_2"

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_mount_unmount_2 SYNC"
