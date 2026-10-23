#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: cas is an object-storage metadata type, absent from the fasttest image.

# Exit code of `clickhouse-disks --query`: upstream commands keep the "always 0" contract
# (04812 relies on it); a `cas-*` command that throws makes the process exit 1, whatever runs after it.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="ca_exit_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_disks_exit_${RANDOM}"
LOCAL_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_local"
rm -rf "${POOL_DIR:?}" "${LOCAL_DIR:?}"
mkdir -p "${POOL_DIR}" "${LOCAL_DIR}"
export TEST_DISK_05393_POOL="${POOL_DIR}/"
export TEST_DISK_05393_LOCAL="${LOCAL_DIR}/"
config="$CUR_DIR/05393_cas_disks_exit_code.xml"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS t_disks_exit SYNC;
CREATE TABLE t_disks_exit (k UInt64, v String)
ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05393',
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/',
    cas_gc_enabled = 0);
INSERT INTO t_disks_exit SELECT number, toString(number) FROM numbers(1000);
EOF

# Prints "<label> <exit code>"; the command's own output is not part of the oracle.
function disks()
{
    local label="$1" disk="$2" query="$3"
    clickhouse-disks -C "$config" --disk "$disk" --query "$query" > /dev/null 2>&1
    echo "$label $?"
}

disks clean_fsck cas_ro_05393 "cas-fsck"

# A canonical blob key nothing references: a copy of a live blob under another hash.
blob=$(find "${POOL_DIR}" -path '*/blobs/*' -type f ! -name '*.meta' | head -1)
stray="$(dirname "$blob")/$(basename "$blob" | tr '0-9a-f' '1-9a-f0')"
cp "$blob" "$stray"
disks unreachable_fsck cas_ro_05393 "cas-fsck"

# A blob a committed manifest names is gone.
rm -f "$blob"
disks dangling_fsck cas_ro_05393 "cas-fsck"

# An upstream command that throws keeps the upstream contract.
disks remove_missing local_05393 "remove no_such_path_05393"

# One failing cas-* command decides the exit code of the whole query.
disks fsck_then_list cas_ro_05393 "cas-fsck; list ."

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_disks_exit SYNC"
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal --query "SYSTEM CAS FORGET '${DISK_NAME}'"
rm -rf "${POOL_DIR:?}" "${LOCAL_DIR:?}"
