#!/usr/bin/env bash
# Tags: no-fasttest, no-msan
# Tag no-fasttest: SeaweedFS on localhost:11111
# Tag no-msan: delta-kernel-rs is not built with MSan

# A finished local-read probe drops the Delta iterator because its estimated key count is unknown.
# `_data_lake_snapshot_version` is still taken from that iterator, so the local read must keep the
# snapshot the cluster read reports.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_DATABASE}_05431_delta/"
LOG_NAME="${CLICKHOUSE_DATABASE}_05431_delta_log.json"

cat > "${USER_FILES_PATH}/${LOG_NAME}" <<'EOF'
{"protocol":{"minReaderVersion":1,"minWriterVersion":2}}
{"metaData":{"id":"05431","format":{"provider":"parquet","options":{}},"schemaString":"{\"type\":\"struct\",\"fields\":[{\"name\":\"x\",\"type\":\"integer\",\"nullable\":false,\"metadata\":{}}]}","partitionColumns":[],"configuration":{},"createdTime":1700000000000}}
EOF

${CLICKHOUSE_CLIENT} --query "
INSERT INTO FUNCTION s3('${URL}_delta_log/00000000000000000000.json', 'RawBLOB', 's String')
SELECT * FROM file('${LOG_NAME}', 'RawBLOB', 's String')
SETTINGS s3_truncate_on_insert = 1"

${CLICKHOUSE_CLIENT} --allow_experimental_delta_lake_writes=1 --query "
INSERT INTO FUNCTION deltaLake('${URL}', NOSIGN) SELECT toInt32(1) AS x"

${CLICKHOUSE_CLIENT} --query "
SELECT _data_lake_snapshot_version
FROM deltaLake('${URL}', NOSIGN)
SETTINGS send_logs_level = 'fatal'"

${CLICKHOUSE_CLIENT} --query "
SELECT x, _data_lake_snapshot_version
FROM deltaLakeCluster('test_shard_localhost', '${URL}', NOSIGN)
SETTINGS
    object_storage_cluster_local_read_max_tasks = 10,
    object_storage_cluster_local_read_probe_ms = 5000,
    parallel_replicas_for_cluster_engines = 0,
    send_logs_level = 'fatal',
    log_comment = '05431_delta_local'"

${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log"
${CLICKHOUSE_CLIENT} --query "
SELECT ProfileEvents['Shards']
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05431_delta_local'
    AND type = 'QueryFinish'
ORDER BY event_time_microseconds DESC
LIMIT 1"

rm -f "${USER_FILES_PATH}/${LOG_NAME}"
