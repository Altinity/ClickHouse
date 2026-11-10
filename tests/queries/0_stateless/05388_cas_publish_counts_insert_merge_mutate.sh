#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# One manifest per part on a content-addressed disk: the part is published once under its
# temporary name and renamed with one owner move. ref_drop is not counted: outdated parts are removed by
# the cleanup thread on its own schedule. `min_bytes_for_full_part_storage = 0` pins off packed parts:
# packed parts on a content-addressed disk are a known separate defect.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_pc1', name = '${CLICKHOUSE_DATABASE}_pc1', path = '${CLICKHOUSE_DATABASE}_pc1_pool/')"

# Per manifest (the object_hash shared by manifest_put, build_publish and ref_move): exactly one of each,
# and the publish is under a temporary name. Background merges may add parts between the steps, so the
# part count is a lower bound.
counts() {
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    ${CLICKHOUSE_CLIENT} -q "SELECT '$1', count() >= $2 AS parts_ok, min(bp = 1 AND mp = 1 AND mv = 1) AS all_one FROM
        (SELECT object_hash,
                countIf(event_type = 'build_publish' AND startsWith(ref_name, 'tmp_')) AS bp,
                countIf(event_type = 'manifest_put') AS mp,
                countIf(event_type = 'ref_move') AS mv
         FROM system.cas_log
         WHERE disk_name = '${CLICKHOUSE_DATABASE}_pc1'
           AND event_type IN ('build_publish', 'manifest_put', 'ref_move')
         GROUP BY object_hash)"
}

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_pc1 SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_pc1 (a UInt64, v UInt64, PROJECTION p1 (SELECT v % 10, sum(a) GROUP BY v % 10)) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_pc1"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_pc1 SELECT number, number FROM numbers(1000)"
counts insert 1
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_pc1 SELECT number + 1000, number FROM numbers(1000)"
${CLICKHOUSE_CLIENT} -q "SYSTEM START MERGES t_pc1"
${CLICKHOUSE_CLIENT} -q "OPTIMIZE TABLE t_pc1 FINAL"
counts optimize 3
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_pc1 UPDATE v = v + 1 WHERE 1 SETTINGS mutations_sync = 2"
counts mutate 4
# A mutation that touches no row clones the part (hardlinks) instead of rewriting it: still one
# publish under a temporary name and one owner move.
${CLICKHOUSE_CLIENT} -q "ALTER TABLE t_pc1 DELETE WHERE a < 0 SETTINGS mutations_sync = 2"
counts untouched_mutate 5
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_pc1 SYNC"

# The renamed and merged parts leave nothing behind: drain the retire pipeline (bounded, by header name),
# then every FSCK finding column reads zero.
PENDING=1
for _ in $(seq 1 60); do
    PENDING=$(${CLICKHOUSE_CLIENT} --query "SYSTEM CAS GC RUN '${CLICKHOUSE_DATABASE}_pc1'" --format TSVWithNames \
        | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next } { print $col["pending_condemned"] }')
    [ "${PENDING}" = "0" ] && break
    sleep 0.5
done
echo "gc_drained $([ "${PENDING}" = "0" ] && echo 1 || echo 0)"

${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK '${CLICKHOUSE_DATABASE}_pc1'" --format TSVWithNames \
    | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                  { n = split("dangling unreachable unaccounted stale_edge corrupted_runs chain_broken lifeless_keys", names, " ");
                    for (i = 1; i <= n; i++) print "fsck_" names[i], $col[names[i]] }'
