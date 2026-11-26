#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# Tag no-replicated-database: IcebergLocal is non-replicated.

# The schema evolution of a data file must be validated before any shortcut answers without reading the file:
# the constant columns taken from the file statistics (`allow_experimental_iceberg_read_optimization`)
# and the cached row count (`use_cache_for_count_from_files`) both skip the schema transformation.
# Metadata that violates the Iceberg specification must raise `ICEBERG_SPECIFICATION_VIOLATION`
# whether or not the row count of the data file is cached, and not `BAD_GET` or a result.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

CLIENT="${CLICKHOUSE_CLIENT} --iceberg_metadata_staleness_ms=0 --use_cache_for_count_from_files=1 --optimize_count_from_files=1 --allow_experimental_iceberg_read_optimization=1"

# Rewrites the latest metadata file as a new version with a new current schema.
# `required`: adds a required column, which the specification forbids.
# `struct`: turns the primitive column `p` into a struct, which the specification forbids.
function evolve_metadata()
{
    local path=$1
    local mode=$2
    local latest
    latest=$(ls "${path}metadata/" | grep -E '^v[0-9]+\.metadata\.json$' | sort -t v -k2 -n | tail -1)
    python3 - "${path}metadata" "${latest}" "${mode}" <<'PY'
import copy, json, os, re, sys

metadata_dir, latest_file, mode = sys.argv[1], sys.argv[2], sys.argv[3]

with open(os.path.join(metadata_dir, latest_file)) as fh:
    metadata = json.load(fh)

schema = copy.deepcopy(metadata["schemas"][0])
schema["schema-id"] = 1
if mode == "required":
    schema["fields"].append({"id": 3, "name": "extra", "required": True, "type": "long"})
    metadata["last-column-id"] = 3
else:
    for field in schema["fields"]:
        if field["name"] == "p":
            field["type"] = {"type": "struct", "fields": [{"id": 5, "name": "a", "required": False, "type": "long"}]}
    metadata["last-column-id"] = 5
metadata["schemas"].append(schema)
metadata["current-schema-id"] = 1
metadata["last-updated-ms"] = metadata.get("last-updated-ms", 0) + 60000

version = int(re.match(r"v(\d+)\.metadata\.json", latest_file).group(1)) + 1
tmp_file = os.path.join(metadata_dir, ".tmp_next")
with open(tmp_file, "w") as fh:
    json.dump(metadata, fh)
os.rename(tmp_file, os.path.join(metadata_dir, f"v{version}.metadata.json"))
PY
}

# Prints `$3` if the query fails with `ICEBERG_SPECIFICATION_VIOLATION`, otherwise the actual outcome.
function expect_violation()
{
    local query=$1
    local message=$2
    local out
    out=$(${CLIENT} --query "${query}" 2>&1)
    if grep -q -F "ICEBERG_SPECIFICATION_VIOLATION" <<< "${out}"
    then
        echo "${message}"
    else
        echo "NOT REJECTED: ${out}"
    fi
}

TABLE1="t1_${CLICKHOUSE_DATABASE}_${RANDOM}"
PATH1="${USER_FILES_PATH}/${TABLE1}/"
TABLE2="t2_${CLICKHOUSE_DATABASE}_${RANDOM}"
PATH2="${USER_FILES_PATH}/${TABLE2}/"
rm -rf "${PATH1}" "${PATH2}" 2>/dev/null

# The data file holds a single row, so the statistics of every column are constant.
${CLICKHOUSE_CLIENT} --query "CREATE TABLE ${TABLE1} (c0 Int32, p Int64) ENGINE = IcebergLocal('${PATH1}')"
${CLICKHOUSE_CLIENT} --query "CREATE TABLE ${TABLE2} (c0 Int32, c1 String) ENGINE = IcebergLocal('${PATH2}')"
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --async_insert=0 --query "INSERT INTO ${TABLE1} VALUES (1, 42)"
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --async_insert=0 --query "INSERT INTO ${TABLE2} VALUES (1, 'a')"

# The cached row count is valid only if the data file is older than the cache entry, compared in whole seconds.
sleep 2

# Read the data files without the trivial count shortcut to fill the row count cache.
${CLIENT} --query "SELECT count() FROM ${TABLE1} SETTINGS optimize_trivial_count_query = 0"
${CLIENT} --query "SELECT count() FROM ${TABLE2} SETTINGS optimize_trivial_count_query = 0"

evolve_metadata "${PATH1}" struct
evolve_metadata "${PATH2}" required

# Every column of the file is constant, so the read needs nothing from the file: it used to be answered
# from the cached row count and the statistics of the old schema, and failed with `BAD_GET`.
expect_violation "SELECT * FROM ${TABLE1}" "primitive to complex rejected"

# It used to be answered from the cached row count, without validating the new schema.
expect_violation "SELECT count() FROM ${TABLE2} SETTINGS optimize_trivial_count_query = 0" "required column rejected"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE1}"
${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE2}"
rm -rf "${PATH1}" "${PATH2}" 2>/dev/null

echo "OK"
