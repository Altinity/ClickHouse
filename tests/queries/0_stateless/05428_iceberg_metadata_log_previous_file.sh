#!/usr/bin/env bash
# Tags: no-fasttest
# - no-fasttest: requires `IcebergLocal` (USE_AVRO build option)

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# Each commit must append the metadata file it replaces to `metadata-log`.

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

trap 'rm -rf "${TABLE_PATH}" 2>/dev/null' EXIT

# Prints the newest metadata file and whether it logs the one before it.
function check_metadata_log()
{
    python3 - "${TABLE_PATH}metadata" <<'PY'
import json, os, re, sys
metadata_dir = sys.argv[1]
files = sorted((f for f in os.listdir(metadata_dir) if re.fullmatch(r"v\d+\.metadata\.json", f)), key=lambda f: int(f[1:].split(".")[0]))
metadata_log = json.load(open(os.path.join(metadata_dir, files[-1])))["metadata-log"]
print(files[-1], os.path.basename(metadata_log[-1]["metadata-file"]) == files[-2])
PY
}

${CLICKHOUSE_CLIENT} --query "CREATE TABLE ${TABLE} (a Int32) ENGINE = IcebergLocal('${TABLE_PATH}', 'Parquet')"
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "INSERT INTO ${TABLE} VALUES (1)"

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "TRUNCATE TABLE ${TABLE}"
check_metadata_log

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "INSERT INTO ${TABLE} VALUES (2)"
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "INSERT INTO ${TABLE} VALUES (3)"
${CLICKHOUSE_CLIENT} --allow_experimental_iceberg_compaction=1 \
    --query "OPTIMIZE TABLE ${TABLE} MANIFEST SETTINGS iceberg_manifest_min_count_to_compact=1"
check_metadata_log

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query "ALTER TABLE ${TABLE} ADD COLUMN b Nullable(Int32)"
check_metadata_log

${CLICKHOUSE_CLIENT} --query "DROP TABLE ${TABLE}"
