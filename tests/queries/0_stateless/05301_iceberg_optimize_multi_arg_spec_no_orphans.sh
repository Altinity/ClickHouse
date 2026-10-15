#!/usr/bin/env bash
# Tags: no-fasttest

# Data-file compaction (`OPTIMIZE TABLE` with `allow_experimental_iceberg_compaction`) partitions
# rewritten files by the default partition spec only after it has written them. A multi-argument
# (`source-ids`) transform cannot be evaluated on write, so such a spec must be rejected before any
# data file is written; otherwise the rewritten files are left orphaned in the table directory.
#
# The test creates a table with position deletes (so compaction has work to do), switches the
# default spec to a multi-argument `bucket` transform by patching the latest metadata file, and
# checks that `OPTIMIZE` fails without adding any file under `data/`.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
${CLICKHOUSE_CLIENT} --query "
    CREATE TABLE ${TABLE} (x Int64, y String)
    ENGINE = IcebergLocal('${TABLE_PATH}', 'Parquet')
"

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query \
    "INSERT INTO ${TABLE} SELECT number, toString(number) FROM numbers(5)"

# Merge-on-read delete: writes a position-delete file, which makes data-file compaction non-trivial.
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --query \
    "ALTER TABLE ${TABLE} DELETE WHERE x IN (1, 3)"

# Make a multi-argument bucket over (x, y) the default partition spec. Existing files keep spec 0.
python3 - "${TABLE_PATH}" << 'EOF'
import glob
import json
import re
import sys

files = glob.glob(sys.argv[1] + "metadata/*.metadata.json")
assert files, "no metadata files found"

def version(path):
    match = re.search(r"(\d+)[^/]*\.metadata\.json$", path)
    return int(match.group(1)) if match else -1

latest = max(files, key=version)
with open(latest) as f:
    metadata = json.load(f)

field_ids = {field["name"]: field["id"] for schema in metadata["schemas"] for field in schema["fields"]}
metadata["partition-specs"].append({
    "spec-id": 1,
    "fields": [{"source-ids": [field_ids["x"], field_ids["y"]], "transform": "bucket[16]", "name": "xy_bucket", "field-id": 1000}],
})
metadata["default-spec-id"] = 1
metadata["last-partition-id"] = max(metadata.get("last-partition-id", 999), 1000)
with open(latest, "w") as f:
    json.dump(metadata, f)
EOF

count_data_files()
{
    find "${TABLE_PATH}data" -type f | wc -l
}

files_before=$(count_data_files)

${CLICKHOUSE_CLIENT} --use_iceberg_metadata_files_cache=0 --allow_experimental_iceberg_compaction=1 \
    --query "OPTIMIZE TABLE ${TABLE}" 2>&1 \
    | grep -o -m1 'Multi-argument partition transforms (source-ids) are not supported for writes'

files_after=$(count_data_files)
if [ "${files_before}" -eq "${files_after}" ]; then
    echo "no orphaned data files"
else
    echo "FAIL: data files before OPTIMIZE: ${files_before}, after: ${files_after}"
fi

${CLICKHOUSE_CLIENT} --use_iceberg_metadata_files_cache=0 --query "SELECT x FROM ${TABLE} ORDER BY x"

${CLICKHOUSE_CLIENT} --query "DROP TABLE ${TABLE}"
rm -rf "${TABLE_PATH}"
