#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# no-fasttest: cas is an object-storage metadata type, absent from the fasttest image.
# no-replicated-database: replicas share `user_files`, and `FORGET` certifies the teardown of this server only.

# `cas-fsck --partial` that is cut short by its deadline prints the partial summary and still exits 1 with
# TIMEOUT_EXCEEDED. `--timeout 1` is below the 30 s the disk budgets for one request attempt, so the deadline
# cuts the scan short whatever the pool size or the machine speed.
# The scans run against a copy of the pool: an offline tool must not read the directory of a running server.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="ca_fsck_partial_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
# Fresh directories: a leftover pool of an earlier run may still be mounted by the server and is never touched.
POOL_DIR=$(mktemp -d "${CLICKHOUSE_USER_FILES_UNIQUE}_fsck_partial_XXXXXX") || { echo "mktemp failed"; exit 1; }
COPY_DIR=$(mktemp -d "${CLICKHOUSE_USER_FILES_UNIQUE}_fsck_partial_copy_XXXXXX") || { echo "mktemp failed"; exit 1; }
chmod 755 "${POOL_DIR}" "${COPY_DIR}"
OUT="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_fsck.out"
ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_fsck.err"
trap 'rm -rf "$OUT" "$ERR" "${COPY_DIR:?}"' EXIT
export TEST_DISK_05488_POOL="${COPY_DIR}/"
config="$CUR_DIR/05488_cas_fsck_partial_scan_exit_status.xml"

${CLICKHOUSE_CLIENT} --multiquery <<SQL
DROP TABLE IF EXISTS t_fsck_partial SYNC;
CREATE TABLE t_fsck_partial (k UInt64, v String)
ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_unsafe_remount_no_delay = 1,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05488',
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/',
    cas_gc_enabled = 0);
INSERT INTO t_fsck_partial SELECT number, toString(number) FROM numbers(1000);
SQL
[ $? -eq 0 ] || { echo "fixture setup failed: leaving the pool directory in place"; exit 1; }

cp -r "${POOL_DIR}/." "${COPY_DIR}/" || { echo "pool copy failed: leaving the pool directory in place"; exit 1; }

# Prints "<label> <exit code> <summary lines> <summary lines with partial=1> <stderr lines with TIMEOUT_EXCEEDED>".
function fsck()
{
    local label="$1"
    shift
    clickhouse-disks -C "$config" --disk cas_ro_05488 --query "cas-fsck $*" > "$OUT" 2> "$ERR"
    local rc=$?
    echo "$label $rc $(grep -c 'reachable=' "$OUT") $(grep -c 'partial=1' "$OUT") $(grep -c 'TIMEOUT_EXCEEDED' "$ERR")"
}

fsck complete
# The complete scan must have walked the table's ref records, not merely printed a summary.
echo "complete walked ref records: $(grep -c 'ref_records_walked=[1-9]' "$OUT")"
fsck cut_partial --timeout 1 --partial
fsck cut_without_partial --timeout 1
rm -rf "$OUT" "$ERR" "${COPY_DIR:?}"

# The pool directory is removed only after the disk is forgotten; any failure leaves it in place.
${CLICKHOUSE_CLIENT} --query "DROP TABLE t_fsck_partial SYNC" || {
    echo "DROP failed: leaving the pool directory in place"; exit 1; }
# FORGET logs an operator WARNING that the harness would flag; silence it for this call only.
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISK_NAME}'" || {
    echo "FORGET failed: leaving the pool directory in place"; exit 1; }
LIFECYCLE=$(${CLICKHOUSE_CLIENT} --query "
    SELECT lifecycle || '(' || lifecycle_reason || ')' FROM system.cas_mounts
    WHERE disk = '${DISK_NAME}'") || {
    echo "lifecycle query failed: leaving the pool directory in place"; exit 1; }
[ "${LIFECYCLE}" = "vanished(forgotten)" ] || {
    echo "unexpected lifecycle after FORGET: ${LIFECYCLE}"; exit 1; }
rm -rf "${POOL_DIR:?}"
