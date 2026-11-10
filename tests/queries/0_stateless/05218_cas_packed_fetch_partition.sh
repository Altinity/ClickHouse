#!/usr/bin/env bash
# Tags: no-fasttest, no-shared-merge-tree, no-replicated-database

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="${CLICKHOUSE_DATABASE}_05218_cas_packed_fetch"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS cas_packed_fetch_src SYNC;
DROP TABLE IF EXISTS cas_packed_fetch_dst SYNC;

CREATE TABLE cas_packed_fetch_src (key UInt64, value UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/cas_packed_fetch_src', 'r1')
PARTITION BY key % 2
ORDER BY key
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05218',
    name = '${DISK_NAME}',
    path = '${CLICKHOUSE_DATABASE}_05218_cas_packed_fetch_pool/',
    cas_allow_packed_parts = 1),
    min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = '1G';

CREATE TABLE cas_packed_fetch_dst (key UInt64, value UInt64)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/cas_packed_fetch_dst', 'r1')
PARTITION BY key % 2
ORDER BY key
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05218',
    name = '${DISK_NAME}',
    path = '${CLICKHOUSE_DATABASE}_05218_cas_packed_fetch_pool/',
    cas_allow_packed_parts = 1),
    min_bytes_for_full_part_storage = '1G',
    min_bytes_for_wide_part = '1G';

INSERT INTO cas_packed_fetch_src VALUES (0, 10), (2, 20), (4, 30);

SELECT 'source_data', count(), sum(key), sum(value) FROM cas_packed_fetch_src;
SELECT 'source_storage', any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_fetch_src' AND active;

ALTER TABLE cas_packed_fetch_dst FETCH PARTITION 0
FROM '/clickhouse/tables/{database}/cas_packed_fetch_src'
SETTINGS insert_keeper_fault_injection_probability = 0;

SELECT 'detached_parts', count()
FROM system.detached_parts
WHERE database = currentDatabase() AND table = 'cas_packed_fetch_dst';

ALTER TABLE cas_packed_fetch_dst ATTACH PARTITION 0
SETTINGS insert_keeper_fault_injection_probability = 0;

SELECT 'detached_after_attach', count()
FROM system.detached_parts
WHERE database = currentDatabase() AND table = 'cas_packed_fetch_dst';
SELECT 'destination_data', count(), sum(key), sum(value) FROM cas_packed_fetch_dst;
SELECT 'destination_row', key, value FROM cas_packed_fetch_dst ORDER BY key;
SELECT 'destination_storage', any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_packed_fetch_dst' AND active;

DROP TABLE cas_packed_fetch_src SYNC;
DROP TABLE cas_packed_fetch_dst SYNC;
EOF

${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISK_NAME}'"
