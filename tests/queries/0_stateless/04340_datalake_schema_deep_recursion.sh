#!/usr/bin/env bash
# Tags: no-fasttest, no-msan
# Regression test: Iceberg parses table-metadata JSON with Poco::JSON::Parser, which defaulted to
# unlimited depth (setDepth was a no-op), so a deeply nested metadata file overflowed the native
# stack inside Poco's recursive parser. The parser now sets a depth limit, so deep nesting is
# rejected with a clean JSON exception instead of crashing.
#
# The DeltaLake half of the upstream test is dropped here: the Antalya fork does not register the
# deltaLakeLocal table function (only icebergLocal exists), so there is no lightweight local path to
# exercise the C++ DeltaLake metadata parser. Iceberg covers the same shared Poco depth-limit fix.
# Tag no-msan is retained from the original test (kept to avoid changing which CI configs run this).

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TMP_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT

# Reads a query's combined output from stdin. If it contains $2, print a stable "<label>: <marker>"
# line; otherwise print the actual output so a CI failure shows what really happened instead of FAIL.
expect_contains() {
    local label="$1" marker="$2" out
    out=$(cat)
    if printf '%s\n' "$out" | grep -qF "$marker"; then
        echo "$label: $marker"
    else
        echo "$label: expected '$marker', got:"
        printf '%s\n' "$out" | head -3
    fi
}

# Iceberg parses the whole metadata JSON with Poco before any field validation, so a deeply nested
# metadata file (a struct nested 2000 deep, exceeding the parser depth limit of 1000) overflows the
# parser and must be rejected with a JSON exception, not crash the process.
# allow_local_data_lakes=1 lifts the Altinity guard that otherwise disables the *Local table functions.
mkdir -p "$TMP_DIR/ice/metadata"
python3 -c "
N = 2000
open('$TMP_DIR/ice/metadata/v1.metadata.json', 'wb').write(b'{' + b'\"a\":{' * N + b'\"x\":1' + b'}' * N + b'}')
open('$TMP_DIR/ice/metadata/version-hint.text', 'w').write('1')
"
$CLICKHOUSE_LOCAL --query "DESCRIBE TABLE icebergLocal('$TMP_DIR/ice') SETTINGS allow_local_data_lakes=1 FORMAT Null" 2>&1 \
    | expect_contains iceberg_depth JSONException
