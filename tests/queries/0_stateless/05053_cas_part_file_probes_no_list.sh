#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# Loading a table on a cas disk must not issue one S3 LIST per part file: checkSize asks
# existsDirectory for every checksum entry of every part, and that probe used to LIST the table's
# _files/ prefix each time. ATTACH reloads every part synchronously inside the query, so the ATTACH
# query's own ProfileEvents (unaffected by parallel tests and by background work) are the oracle:
# the LIST count of a 200-part table equals that of a 20-part table and stays below the part count.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_cas95',
    name = '${CLICKHOUSE_DATABASE}_cas95_cas',
    path = '${CLICKHOUSE_DATABASE}_cas95_cas_pool/')"

for n in 20 200; do
    ${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_$n"
    ${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_$n (a UInt64) ENGINE = MergeTree ORDER BY a
        SETTINGS disk = $DISK, max_bytes_to_merge_at_max_space_in_pool = 1"
    # One row per block, one block per part: n parts from one INSERT.
    ${CLICKHOUSE_CLIENT} -q "INSERT INTO t_$n SELECT number FROM numbers($n)
        SETTINGS max_block_size = 1, min_insert_block_size_rows = 1, min_insert_block_size_bytes = 1"
    ${CLICKHOUSE_CLIENT} -q "SELECT 'parts_$n', count() FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$n' AND active"
    ${CLICKHOUSE_CLIENT} -q "DETACH TABLE t_$n"
    ${CLICKHOUSE_CLIENT} --query_id "${CLICKHOUSE_DATABASE}_attach_$n" -q "ATTACH TABLE t_$n"
    ${CLICKHOUSE_CLIENT} -q "SELECT 'rows_after_attach_$n', count() FROM t_$n"
done

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log"

${CLICKHOUSE_CLIENT} -q "
WITH
    (SELECT ProfileEvents['CASRootList'] FROM system.query_log
      WHERE current_database = currentDatabase() AND type = 'QueryFinish'
        AND query_id = '${CLICKHOUSE_DATABASE}_attach_20' ORDER BY event_time DESC LIMIT 1) AS l20,
    (SELECT ProfileEvents['CASRootList'] FROM system.query_log
      WHERE current_database = currentDatabase() AND type = 'QueryFinish'
        AND query_id = '${CLICKHOUSE_DATABASE}_attach_200' ORDER BY event_time DESC LIMIT 1) AS l200
SELECT 'lists_equal', l200 = l20, 'lists_below_parts', l200 < 20, 'lists_positive', l20 > 0"

${CLICKHOUSE_CLIENT} -q "DROP TABLE t_20"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_200"
${CLICKHOUSE_CLIENT} -q "SELECT 'dropped_ok'"

# FORGET logs an operator WARNING (the decommission is deliberately prominent in the server log); the
# clickhouse-test harness runs the client at --send_logs_level=warning, which would stream that expected
# warning to stderr and be flagged as a failure. Suppress it for the FORGET call only.
${CLICKHOUSE_CLIENT} --send_logs_level=fatal -q "SYSTEM CAS FORGET '${CLICKHOUSE_DATABASE}_cas95_cas'"
