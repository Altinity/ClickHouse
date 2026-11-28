#!/usr/bin/env bash
# Tags: no-fasttest
# A schema added after `current-schema-id` was rolled back to a lower schema must not record an annotation
# that became stale after another engine evolved the field.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}_rollback"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"
CLIENT="${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --iceberg_metadata_staleness_ms=0"

${CLIENT} --query "
    CREATE TABLE ${TABLE} (k Int32, x SimpleAggregateFunction(max, Int32))
    ENGINE = IcebergLocal('${TABLE_PATH}')
    SETTINGS allow_aggregate_function_states_in_open_formats = 1"
${CLIENT} --query "ALTER TABLE ${TABLE} ADD COLUMN y Nullable(Int32)"
${CLIENT} --query "ALTER TABLE ${TABLE} ADD COLUMN z Nullable(Int32)"

# Another engine promoted `x` to `long` in schemas 1 and 2, cleared its annotation in schema 2 and rolled back to schema 1.
LATEST=$(ls "${TABLE_PATH}metadata/" | grep -E '^v[0-9]+\.metadata\.json$' | sort -t v -k2 -n | tail -1)
python3 - "${TABLE_PATH}metadata" "${LATEST}" <<'PY'
import json, os, re, sys
metadata_dir, latest_file = sys.argv[1], sys.argv[2]
with open(os.path.join(metadata_dir, latest_file)) as f:
    metadata = json.load(f)
for schema in metadata["schemas"]:
    if schema["schema-id"] > 0:
        next(field for field in schema["fields"] if field["id"] == 2)["type"] = "long"
metadata["properties"]["clickhouse.type.2.2"] = ""
metadata["current-schema-id"] = 1
version = int(re.match(r"v(\d+)\.metadata\.json", latest_file).group(1)) + 1
with open(os.path.join(metadata_dir, f"v{version}.metadata.json"), "w") as f:
    json.dump(metadata, f)
PY

${CLIENT} --query "ALTER TABLE ${TABLE} ADD COLUMN w Nullable(Int32)"
${CLIENT} --query "DESCRIBE TABLE ${TABLE}" | cut -f1,2

${CLIENT} --query "DROP TABLE ${TABLE}"
rm -rf "${TABLE_PATH}"
