#!/usr/bin/env bash
# Tags: no-fasttest

# The Iceberg schema records only the type and the order of columns, so `ADD COLUMN` clauses
# other than the type and `FIRST` / `AFTER` must be rejected rather than silently dropped.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
${CLICKHOUSE_CLIENT} --query "
    CREATE TABLE ${TABLE} (c0 Int)
    ENGINE = IcebergLocal('${TABLE_PATH}')
"
# To have at least one real snapshot. Otherwise alter can be noop.
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "INSERT INTO ${TABLE} VALUES (1)"

for alter in \
    "ADD COLUMN z Nullable(Int32) DEFAULT 7 FIRST" \
    "ADD COLUMN z Nullable(Int32) ALIAS c0 AFTER c0" \
    "ADD COLUMN z Nullable(Int32) COMMENT 'x' FIRST"
do
    echo "${alter}"
    ${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "ALTER TABLE ${TABLE} ${alter}" 2>&1 | grep -o -m1 "NOT_IMPLEMENTED"
done

${CLICKHOUSE_CLIENT} --query "DESCRIBE TABLE ${TABLE}" | cut -f1,2

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
rm -rf "${TABLE_PATH}"
