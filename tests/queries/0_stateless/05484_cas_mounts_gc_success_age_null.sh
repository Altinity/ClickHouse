#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# Before this server's GC has succeeded once, `system.cas_mounts.last_success_age_seconds` is NULL and the
# asynchronous metric `CASGCLastSuccessAgeSeconds_<disk>` is absent; both appear after the first round.
# The long `cas_gc_interval_sec` keeps the background loop from running a round before `SYSTEM CAS GC RUN`.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="${CLICKHOUSE_DATABASE}_05484_cas_age_null"

${CLICKHOUSE_CLIENT} --multiline -q """
DROP TABLE IF EXISTS t_cas_gc_success_age_null;

CREATE TABLE t_cas_gc_success_age_null (a UInt64)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_05484',
    name = '${DISK}',
    path = '${DISK}_pool/',
    cas_gc_enabled = 1,
    cas_gc_interval_sec = 3600);

INSERT INTO t_cas_gc_success_age_null SELECT number FROM numbers(100);
"""

query_age="SELECT last_success_age_seconds IS NULL, last_success_age_seconds < 60 FROM system.cas_mounts WHERE disk LIKE '%${DISK}%' AND is_leader IS NOT NULL"
query_metric="SELECT count() FROM system.asynchronous_metrics WHERE metric = 'CASGCLastSuccessAgeSeconds_${DISK}'"

echo 'before the first round'
${CLICKHOUSE_CLIENT} -q "$query_age"
${CLICKHOUSE_CLIENT} -q "SYSTEM RELOAD ASYNCHRONOUS METRICS"
${CLICKHOUSE_CLIENT} -q "$query_metric"

${CLICKHOUSE_CLIENT} -q "SYSTEM CAS GC RUN '${DISK}'" > /dev/null

echo 'after the first round'
${CLICKHOUSE_CLIENT} -q "$query_age"
${CLICKHOUSE_CLIENT} -q "SYSTEM RELOAD ASYNCHRONOUS METRICS"
${CLICKHOUSE_CLIENT} -q "$query_metric"

${CLICKHOUSE_CLIENT} -q "DROP TABLE t_cas_gc_success_age_null"

# FORGET logs an operator WARNING that the harness would flag; silence it for this call only.
${CLICKHOUSE_CLIENT} --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CAS FORGET '${DISK}'" || {
    echo "FORGET failed"; exit 1; }
