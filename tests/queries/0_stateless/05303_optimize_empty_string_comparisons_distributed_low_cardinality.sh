#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# - no-fasttest: requires `IcebergLocal` (USE_AVRO).
# - no-replicated-database: `IcebergLocal` is non-replicated.

# A `Distributed` table may declare `LowCardinality(String)` over a plain `String` column.
# With the analyzer and `optimize_empty_string_comparisons`, `s != ''` is rewritten to `notEmpty`
# only for `String`. `object_storage_cluster` re-analyzes that expression against the underlying
# `IcebergLocal` column, so the initiator header and the remote block disagree
# (`NOT_FOUND_COLUMN_IN_BLOCK`). Two of the four values are non-empty, so every query must return 2.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE_PATH="${CLICKHOUSE_USER_FILES_UNIQUE}/t_ice/"
rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE}"
mkdir -p "${TABLE_PATH}"
trap 'rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE}"' EXIT

${CLICKHOUSE_CLIENT} --query "
DROP TABLE IF EXISTS t_dist;
DROP TABLE IF EXISTS t_dist_str;
DROP TABLE IF EXISTS t;

CREATE TABLE t (id UInt64, s String)
ENGINE = IcebergLocal('${TABLE_PATH}');
"

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --async_insert=0 --query "
INSERT INTO t VALUES (1, 'abc'), (2, ''), (3, 'def'), (4, '');
"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_dist (id UInt64, s LowCardinality(String))
ENGINE = Distributed('test_shard_localhost', currentDatabase(), t);

CREATE TABLE t_dist_str (id UInt64, s String)
ENGINE = Distributed('test_shard_localhost', currentDatabase(), t);
"

# Declared `LowCardinality(String)`, real column `String`. Only analyzer + the optimization + cluster dispatch fails today.
${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t_dist
SETTINGS enable_analyzer = 1, optimize_empty_string_comparisons = 1, object_storage_cluster = 'test_shard_localhost'
"

${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t_dist
SETTINGS enable_analyzer = 0, optimize_empty_string_comparisons = 1, object_storage_cluster = 'test_shard_localhost'
"

${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t_dist
SETTINGS enable_analyzer = 1, optimize_empty_string_comparisons = 0, object_storage_cluster = 'test_shard_localhost'
"

${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t_dist
SETTINGS enable_analyzer = 0, optimize_empty_string_comparisons = 0, object_storage_cluster = 'test_shard_localhost'
"

${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s = '') FROM t_dist
SETTINGS enable_analyzer = 1, optimize_empty_string_comparisons = 1, object_storage_cluster = 'test_shard_localhost'
"

# Same declared type, and the underlying table itself, stay correct with the optimization enabled.
${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t_dist
SETTINGS enable_analyzer = 1, optimize_empty_string_comparisons = 1
"

${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t
SETTINGS enable_analyzer = 1, optimize_empty_string_comparisons = 1, object_storage_cluster = 'test_shard_localhost'
"

${CLICKHOUSE_CLIENT} --query "
SELECT countIf(s != '') FROM t_dist_str
SETTINGS enable_analyzer = 1, optimize_empty_string_comparisons = 1, object_storage_cluster = 'test_shard_localhost'
"
