#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="${CLICKHOUSE_DATABASE}_05219_cas_packed_backup"
BACKUP_NAME="Disk('backups', '${CLICKHOUSE_TEST_UNIQUE_NAME}')"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS cas_packed_backup SYNC;

CREATE TABLE cas_packed_backup
(
    key UInt64,
    value UInt64,
    PROJECTION p (SELECT key, count(), sum(value) GROUP BY key)
)
ENGINE = MergeTree
ORDER BY key
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05219',
    name = '${DISK_NAME}',
    path = '${CLICKHOUSE_DATABASE}_05219_cas_packed_backup_pool/',
    cas_allow_packed_parts = 1),
    min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = '1G';

INSERT INTO cas_packed_backup VALUES (1, 10), (2, 20), (1, 30), (3, 40), (2, 50);

SELECT 'source_data', count(), sum(key), sum(value) FROM cas_packed_backup;
SELECT 'source_storage', any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_backup' AND active;

BACKUP TABLE cas_packed_backup TO ${BACKUP_NAME} FORMAT Null;
DROP TABLE cas_packed_backup SYNC;
RESTORE TABLE cas_packed_backup FROM ${BACKUP_NAME} FORMAT Null;

SELECT 'restored_data', count(), sum(key), sum(value) FROM cas_packed_backup;
SELECT 'restored_storage', any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_backup' AND active;
SELECT 'projection', key, count(), sum(value)
FROM cas_packed_backup
GROUP BY key
ORDER BY key
SETTINGS optimize_use_projections = 1, force_optimize_projection = 1;

DROP TABLE cas_packed_backup SYNC;
EOF

${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISK_NAME}'"
