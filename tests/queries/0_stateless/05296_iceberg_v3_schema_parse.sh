#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# - no-fasttest: requires `IcebergLocal` (USE_AVRO).
# - no-replicated-database: `IcebergLocal` is non-replicated.

# ClickHouse-written Iceberg v3 tables store schemas in the `schemas` array and have
# no top-level `schema`. Parsing them with the v1 method first logged a false
# spec-violation warning on every read of `DateTime64(9)` / `timestamp_ns`.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE_PATH="${CLICKHOUSE_USER_FILES_UNIQUE}/t_ice/"
rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE}"
mkdir -p "${TABLE_PATH}"
trap 'rm -rf "${CLICKHOUSE_USER_FILES_UNIQUE}"' EXIT

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "
CREATE TABLE t (ts DateTime64(9))
ENGINE = IcebergLocal('${TABLE_PATH}', 'Parquet')
SETTINGS iceberg_format_version = 3
"

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --async_insert=0 --query "
INSERT INTO t VALUES ('2020-01-02 03:04:05.123456789')
"

${CLICKHOUSE_CLIENT} --query "SELECT ts FROM t"
