#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# A packed part with a projection merges on a content-addressed disk: the projection sub-part rides the
# parent's transaction and must neither open nor commit its own. CHECK TABLE and reads through the
# projection are not asserted: the packed projection sub-part fails `checkDataPart` with
# `BAD_SIZE_OF_FILE_IN_DATA_PART` on a content-addressed disk, before and after the merge.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_pk', cas_allow_packed_parts = 1, name = '${CLICKHOUSE_DATABASE}_pk', path = '${CLICKHOUSE_DATABASE}_pk_pool/')"

${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_pk SYNC"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_pk (a UInt64, v UInt64, PROJECTION p1 (SELECT v % 10, sum(a) GROUP BY v % 10)) ENGINE = MergeTree ORDER BY a SETTINGS disk = ${DISK}, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 1000000000"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP MERGES t_pk"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_pk SELECT number, number FROM numbers(1000)"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t_pk SELECT number + 1000, number FROM numbers(1000)"
${CLICKHOUSE_CLIENT} -q "SYSTEM START MERGES t_pk"
${CLICKHOUSE_CLIENT} -q "OPTIMIZE TABLE t_pk FINAL"
${CLICKHOUSE_CLIENT} -q "SELECT 'parts', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_pk' AND active"
${CLICKHOUSE_CLIENT} -q "SELECT 'storage', part_storage_type FROM system.parts WHERE database = currentDatabase() AND table = 't_pk' AND active"
${CLICKHOUSE_CLIENT} -q "SELECT 'rows', count(), sum(a), sum(v) FROM t_pk SETTINGS optimize_use_projections = 0"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_pk SYNC"
