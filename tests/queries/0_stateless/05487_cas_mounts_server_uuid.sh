#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.
# no-replicated-database: replicas share `user_files`, and `FORGET` certifies the teardown of this server only.

# `system.cas_mounts.server_uuid` of this server's own mount equals `serverUUID()`, the content of the uuid file.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# FORGET is terminal for a disk name, so each run takes its own name and pool directory.
DISK="${CLICKHOUSE_DATABASE}_05487_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
# A fresh directory: a leftover pool of an earlier run may still be mounted by the server and is never touched.
POOL_DIR=$(mktemp -d "${CLICKHOUSE_USER_FILES_UNIQUE}_05487_XXXXXX") || { echo "mktemp failed"; exit 1; }
chmod 755 "${POOL_DIR}"

${CLICKHOUSE_CLIENT} --multiline -q """
DROP TABLE IF EXISTS t_cas_mounts_server_uuid;

CREATE TABLE t_cas_mounts_server_uuid (a UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05487',
    name = '${DISK}',
    path = '${POOL_DIR}/');

INSERT INTO t_cas_mounts_server_uuid VALUES (1);
""" || { echo "fixture setup failed: leaving the pool directory in place"; exit 1; }

${CLICKHOUSE_CLIENT} -q "
SELECT count(), countIf(server_uuid = serverUUID())
FROM system.cas_mounts
WHERE disk = '${DISK}' AND is_leader IS NOT NULL"

# SYNC: removes on a forgotten disk are no-ops, so the table must be gone before the disk is forgotten.
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_cas_mounts_server_uuid SYNC" || {
    echo "DROP failed: leaving the pool directory in place"; exit 1; }

# FORGET logs an operator WARNING that the harness would flag; silence it for this call only.
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISK}'" || {
    echo "FORGET failed: leaving the pool directory in place"; exit 1; }
LIFECYCLE=$(${CLICKHOUSE_CLIENT} --query "
    SELECT lifecycle || '(' || lifecycle_reason || ')' FROM system.cas_mounts
    WHERE disk = '${DISK}'") || {
    echo "lifecycle query failed: leaving the pool directory in place"; exit 1; }
[ "${LIFECYCLE}" = "vanished(forgotten)" ] || {
    echo "unexpected lifecycle after FORGET: ${LIFECYCLE}"; exit 1; }
rm -rf "${POOL_DIR:?}"
