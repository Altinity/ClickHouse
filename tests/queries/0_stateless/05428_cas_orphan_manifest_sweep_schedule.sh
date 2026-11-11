#!/usr/bin/env bash
# Tags: no-fasttest

set -e
CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$CUR_DIR"/../shell_config.sh

schedule_default_disk="${CLICKHOUSE_DATABASE}_sweep_default"
schedule_zero_disk="${CLICKHOUSE_DATABASE}_sweep_zero"
schedule_default_pool="${CLICKHOUSE_USER_FILES_UNIQUE}_sweep_default"
schedule_zero_pool="${CLICKHOUSE_USER_FILES_UNIQUE}_sweep_zero"

cleanup()
{
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS sweep_default SYNC; DROP TABLE IF EXISTS sweep_zero SYNC" --multiquery >/dev/null 2>&1 || true
    ${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FORGET '${schedule_default_disk}'" >/dev/null 2>&1 || true
    ${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FORGET '${schedule_zero_disk}'" >/dev/null 2>&1 || true
    rm -rf "${schedule_default_pool:?}" "${schedule_zero_pool:?}"
}
trap cleanup EXIT
mkdir -p "$schedule_default_pool" "$schedule_zero_pool"

for suffix in default zero
do
    if [[ "$suffix" == default ]]
    then
        schedule_disk="$schedule_default_disk"
        schedule_pool="$schedule_default_pool"
        schedule_interval=""
    else
        schedule_disk="$schedule_zero_disk"
        schedule_pool="$schedule_zero_pool"
        schedule_interval=", cas_orphan_manifest_sweep_interval_sec = 0"
    fi
    ${CLICKHOUSE_CLIENT} --query "CREATE TABLE sweep_${suffix} (x UInt64) ENGINE = MergeTree ORDER BY x
        SETTINGS disk = disk(type = object_storage, object_storage_type = local, metadata_type = cas,
            cas_server_root_id = '${schedule_disk}', name = '${schedule_disk}', path = '${schedule_pool}/',
            cas_gc_enabled = 1 ${schedule_interval})"
    ${CLICKHOUSE_CLIENT} --query "SYSTEM CAS GC STOP '${schedule_disk}'; SYSTEM STOP MERGES sweep_${suffix}" --multiquery
    for value in 1 2
    do
        ${CLICKHOUSE_CLIENT} --query "INSERT INTO sweep_${suffix} VALUES (${value})"
        ${CLICKHOUSE_CLIENT} --query "SYSTEM CAS GC RUN '${schedule_disk}'" >/dev/null
        ${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS cas_gc_log"
        ${CLICKHOUSE_CLIENT} --query "SELECT '${suffix}_${value}', phase_metrics['orphan_manifest_sweep_page_listed']
            FROM system.cas_gc_log WHERE disk_name = '${schedule_disk}' AND event_type = 'Phase'
                AND phase = 'orphan_sweep' ORDER BY event_time_microseconds DESC LIMIT 1"
    done
    ${CLICKHOUSE_CLIENT} --query "SELECT '${suffix}_data', count(), sum(x) FROM sweep_${suffix}"
    ${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK '${schedule_disk}'" --format TSVWithNames \
        | awk -F'\t' -v label="${suffix}_fsck" 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "dangling") field = i; next } { print label "\t" $field }'
done
