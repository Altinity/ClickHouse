#!/usr/bin/env bash
# Tags: no-fasttest
# Regression test for https://github.com/ClickHouse/ClickHouse/issues/90731.
# An Iceberg ARRAY column whose name literally contains a dot (e.g. `a.b`) must
# read back its stored values, not an empty array. The mixed case also checks a
# lone dotted column next to several dotted columns that share a prefix
# (`c.x`, `c.y`): all of them must round-trip correctly.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

LONE_PATH="${CLICKHOUSE_USER_FILES}/lakehouses/${CLICKHOUSE_DATABASE}_iceberg_dotted_lone"
MIXED_PATH="${CLICKHOUSE_USER_FILES}/lakehouses/${CLICKHOUSE_DATABASE}_iceberg_dotted_mixed"
rm -rf "${LONE_PATH}" "${MIXED_PATH}"

# 1) A lone dotted Array column must read its stored values, not [].
${CLICKHOUSE_CLIENT} --query "
    SET allow_experimental_insert_into_iceberg = 1;
    DROP TABLE IF EXISTS t_iceberg_dotted_lone;
    CREATE TABLE t_iceberg_dotted_lone (\`a.b\` Array(String)) ENGINE = IcebergLocal('${LONE_PATH}', 'Parquet');
    INSERT INTO t_iceberg_dotted_lone (\`a.b\`) SELECT ['x', 'y', 'z'];
"
# Read through the Iceberg engine table ...
${CLICKHOUSE_CLIENT} --query "SELECT \`a.b\` FROM t_iceberg_dotted_lone"
# ... and through the icebergLocal table function with an explicitly-declared schema.
${CLICKHOUSE_CLIENT} --query "SELECT \`a.b\` FROM icebergLocal('${LONE_PATH}', 'Parquet', '\`a.b\` Array(String)')"

# 2) A lone dotted column alongside dotted columns that share a prefix (c.x, c.y).
${CLICKHOUSE_CLIENT} --query "
    SET allow_experimental_insert_into_iceberg = 1;
    DROP TABLE IF EXISTS t_iceberg_dotted_mixed;
    CREATE TABLE t_iceberg_dotted_mixed (\`a.b\` Array(String), \`c.x\` Array(Int32), \`c.y\` Array(String)) ENGINE = IcebergLocal('${MIXED_PATH}', 'Parquet');
    INSERT INTO t_iceberg_dotted_mixed (\`a.b\`, \`c.x\`, \`c.y\`) SELECT ['a', 'b', 'c'], [1, 2], ['p', 'q'];
"
${CLICKHOUSE_CLIENT} --query "SELECT \`a.b\`, \`c.x\`, \`c.y\` FROM t_iceberg_dotted_mixed"
${CLICKHOUSE_CLIENT} --query "SELECT \`a.b\`, \`c.x\`, \`c.y\` FROM icebergLocal('${MIXED_PATH}', 'Parquet', '\`a.b\` Array(String), \`c.x\` Array(Int32), \`c.y\` Array(String)')"

# Cleanup
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_iceberg_dotted_lone"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_iceberg_dotted_mixed"
rm -rf "${LONE_PATH}" "${MIXED_PATH}"
