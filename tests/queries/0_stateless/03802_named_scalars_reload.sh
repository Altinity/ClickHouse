#!/usr/bin/env bash

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh
CLICKHOUSE_CLIENT="$CLICKHOUSE_CLIENT --allow_experimental_named_scalars=1"
CLICKHOUSE_LOCAL="$CLICKHOUSE_LOCAL --allow_experimental_named_scalars=1"

TMP_DIR="${CLICKHOUSE_TMP}/named_scalars_reload_${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
mkdir -p "$TMP_DIR/metadata"

${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --multiquery --query "
CREATE TABLE src (x UInt64) ENGINE=Memory;
INSERT INTO src VALUES (1);
CREATE NAMED SCALAR cv_from_table AS (SELECT max(x) FROM src);
CREATE NAMED SCALAR cv_stale REFRESH EVERY 1 SECOND AS (SELECT toUInt64(now()));
DROP TABLE src;
"

DEFINITIONS_DIR="${TMP_DIR%/}/named_scalars"
VALUES_DIR="${TMP_DIR%/}/named_scalars_cache"
value_file_for() {
    python3 - "$DEFINITIONS_DIR" "$VALUES_DIR" "$1" <<'PY'
import re, sys

definitions_dir, values_dir, name = sys.argv[1], sys.argv[2], sys.argv[3]
definition = open(f"{definitions_dir}/named_scalar_{name}.sql", "r").read()
match = re.search(r"UUID '([^']+)'", definition)
if not match:
    raise SystemExit(f"UUID not found for {name}")
print(f"{values_dir}/{match.group(1).replace('-', '%2D')}.bin")
PY
}

STALE_FILE="$(value_file_for cv_stale)"
FROM_TABLE_FILE="$(value_file_for cv_from_table)"

python3 - "$STALE_FILE" <<'PY'
import re, sys

path = sys.argv[1]
data = open(path, "rb").read()
data = re.sub(rb"^last_successful_update_time: \d+$", b"last_successful_update_time: 0", data, count=1, flags=re.MULTILINE)
open(path, "wb").write(data)
PY

output="$(${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --multiquery --format=TSV --query "
SELECT getNamedScalar('cv_from_table');
SELECT getNamedScalar('cv_stale');
SELECT sleep(3) FORMAT Null;
SELECT getNamedScalar('cv_stale');
")"

IFS=$'\n' read -r -d '' -a lines <<<"${output}"$'\0'
persist="${lines[0]}"
v1="${lines[1]}"
v2="${lines[2]}"

echo "$persist"
if [ "$v2" -gt "$v1" ]; then
    echo 1
else
    echo 0
fi

${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --query "DROP NAMED SCALAR cv_from_table"
if [ -f "${FROM_TABLE_FILE}" ]; then
    echo "value_file_remains"
else
    echo "value_file_removed"
fi
