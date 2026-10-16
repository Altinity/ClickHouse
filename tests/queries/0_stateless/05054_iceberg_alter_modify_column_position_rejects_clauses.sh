#!/usr/bin/env bash
# Tags: no-fasttest

# The Iceberg schema records only the type and the order of columns, so `MODIFY COLUMN` clauses
# other than the type and `FIRST` / `AFTER` must be rejected rather than silently dropped.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
${CLICKHOUSE_CLIENT} --query "
    CREATE TABLE ${TABLE} (c0 Int, c1 Int)
    ENGINE = IcebergLocal('${TABLE_PATH}')
"
# To have at least one real snapshot. Otherwise alter can be noop.
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "INSERT INTO ${TABLE} VALUES (1, 2)"

for alter in \
    "MODIFY COLUMN c1 COMMENT 'x' FIRST" \
    "MODIFY COLUMN c1 Int DEFAULT 1 AFTER c0" \
    "MODIFY COLUMN c1 Int CODEC(ZSTD) FIRST" \
    "MODIFY COLUMN c1 Nullable(Int) COMMENT 'x'"
do
    echo "${alter}"
    ${CLICKHOUSE_CLIENT} --query "ALTER TABLE ${TABLE} ${alter}" 2>&1 | grep -o -m1 "NOT_IMPLEMENTED"
done

${CLICKHOUSE_CLIENT} --query "DESCRIBE TABLE ${TABLE}" | cut -f1,2

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
rm -rf "${TABLE_PATH}"
