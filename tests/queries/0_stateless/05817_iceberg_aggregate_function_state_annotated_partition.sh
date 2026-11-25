#!/usr/bin/env bash
# Tags: no-fasttest
# Iceberg partition values of an annotated `DateTime64(3)` field are microseconds and must not be read at scale 3.
# The annotation is added the way another engine could leave it: to a field that already is a partition source.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -euo pipefail

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}_annotated_partition"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

cleanup()
{
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
    rm -rf "${TABLE_PATH}"
}
trap cleanup EXIT

CLIENT="${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --session_timezone=UTC"

${CLIENT} --query "
    CREATE TABLE ${TABLE} (k UInt32, x DateTime64(6, 'UTC'))
    ENGINE = IcebergLocal('${TABLE_PATH}')
    PARTITION BY x"
${CLIENT} --query "INSERT INTO ${TABLE} VALUES (1, '2026-01-01 00:00:00.123')"
${CLIENT} --query "DROP TABLE ${TABLE}"

# Write the next metadata version with the type annotation of field `x` (id 2) for schema 0.
python3 - "${TABLE_PATH}metadata" <<'EOF'
import glob, json, re, sys
directory = sys.argv[1]
version = max(int(re.search(r'/v(\d+)\.metadata\.json$', f).group(1)) for f in glob.glob(directory + '/v*.metadata.json'))
with open(f'{directory}/v{version}.metadata.json') as f:
    metadata = json.load(f)
metadata.setdefault('properties', {})['clickhouse.type.2.0'] = "SimpleAggregateFunction(max, DateTime64(3, 'UTC'))"
with open(f'{directory}/v{version + 1}.metadata.json', 'w') as f:
    json.dump(metadata, f)
EOF

${CLIENT} --query "SELECT x, toTypeName(x) FROM icebergLocal('${TABLE_PATH}')"
${CLIENT} --query "SELECT count() FROM icebergLocal('${TABLE_PATH}') WHERE x = toDateTime64('2026-01-01 00:00:00.123', 3, 'UTC')"
${CLIENT} --query "SELECT count() FROM icebergLocal('${TABLE_PATH}') WHERE x != toDateTime64('2026-01-01 00:00:00.123', 3, 'UTC')"
