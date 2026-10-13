#!/usr/bin/env bash
# Tags: no-fasttest, no-random-merge-tree-settings

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS cas_packed_local;
DROP TABLE IF EXISTS cas_packed_disabled;

CREATE TABLE cas_packed_local (id UInt64, value String)
ENGINE = MergeTree ORDER BY id
SETTINGS storage_policy = 'default', min_bytes_for_full_part_storage = '1G';

CREATE TABLE cas_packed_disabled (id UInt64, value String)
ENGINE = MergeTree ORDER BY id
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05217_disabled',
    name = '${CLICKHOUSE_DATABASE}_05217_disabled',
    path = '${CLICKHOUSE_DATABASE}_05217_disabled_pool/'),
    min_bytes_for_full_part_storage = '1G';

INSERT INTO cas_packed_local VALUES (1, 'local');
INSERT INTO cas_packed_disabled VALUES (1, 'cas');

SELECT table, part_storage_type
FROM system.parts
WHERE database = currentDatabase()
    AND table IN ('cas_packed_local', 'cas_packed_disabled')
    AND active
ORDER BY table;

DROP TABLE cas_packed_local;
DROP TABLE cas_packed_disabled;

SET send_logs_level = 'fatal';
SYSTEM CAS FORGET '${CLICKHOUSE_DATABASE}_05217_disabled';
EOF
