#!/usr/bin/env bash
# Tags: no-fasttest

# Regression test: ALTER TABLE ... MODIFY COLUMN ... on an Iceberg table
# should not throw a LOGICAL_ERROR exception ("Metadata is not initialized")
# when no SELECT or INSERT preceded the alter in the same server lifetime.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
${CLICKHOUSE_CLIENT} --query "
    CREATE TABLE ${TABLE} (c0 Float64)
    ENGINE = IcebergLocal('${TABLE_PATH}')
"
# To have at least one real snapshot. Otherwise alter can be noop.
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "INSERT INTO ${TABLE} VALUES (1.0)"

# DETACH + ATTACH rebuilds the storage object from the stored CREATE statement,
# so its in-memory metadata has no datalake metadata loaded — the same state as
# after a server restart.
${CLICKHOUSE_CLIENT} --query "DETACH TABLE ${TABLE}"
${CLICKHOUSE_CLIENT} --query "ATTACH TABLE ${TABLE}"

# Without the fix, this throws a LOGICAL_ERROR exception.
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "
    ALTER TABLE ${TABLE} MODIFY COLUMN c0 Nullable(Int64)
"

${CLICKHOUSE_CLIENT} --query "SHOW CREATE TABLE ${TABLE}" | grep -F "c0 Nullable(Int64)"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
rm -rf "${TABLE_PATH}"
