#!/usr/bin/env bash
# Tags: no-parallel
#
# Verify CREATE normalizes unqualified identifiers with the user's current
# database in the persisted definition and refresh / reload resolves them
# without depending on the server/global current database.
#
# A scalar created in `mydb` referring to `t` (unqualified) must persist
# as a self-contained definition over `mydb.t`. Otherwise a later refresh
# from a process whose current database is `default` would fail or retarget.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh
CLICKHOUSE_CLIENT="$CLICKHOUSE_CLIENT --allow_experimental_named_scalars=1"
CLICKHOUSE_LOCAL="$CLICKHOUSE_LOCAL --allow_experimental_named_scalars=1"

TMP_DIR="${CLICKHOUSE_TMP}/named_scalars_db_${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"

# Run 1a: bootstrap database + table in `mydb` (MergeTree so it persists
# across clickhouse-local invocations against the same --path).
${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --multiquery --query "
CREATE DATABASE IF NOT EXISTS mydb;
CREATE TABLE mydb.t (x UInt64) ENGINE=MergeTree() ORDER BY x;
INSERT INTO mydb.t VALUES (10), (20), (30);
" 2>&1

# Run 1b: CREATE NAMED SCALAR with `--database=mydb` so the session's
# currentDatabase is `mydb` at CREATE time. The scalar references `t`
# unqualified. CREATE-time normalization must rewrite it to `mydb.t`.
# Use REFRESH so we can force a re-evaluation in run 2 with a different
# default database.
${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --database=mydb --multiquery --query "
CREATE NAMED SCALAR cv_db REFRESH EVERY 36500 DAYS AS (SELECT sum(x) FROM t);
SELECT sleep(1) FORMAT Null;
SELECT getNamedScalar('cv_db');
" 2>&1

# Run 2: open the same data dir with --database=default. If the persisted
# definition still contained unqualified `t`, SYSTEM REFRESH would resolve
# it against `default` (table not there) and fail with UNKNOWN_TABLE.
# With normalization, refresh re-runs against `mydb.t` and returns 60.
${CLICKHOUSE_LOCAL} --path "$TMP_DIR" --database=default --multiquery --query "
SYSTEM REFRESH NAMED SCALAR cv_db;
SELECT sleep(1) FORMAT Null;
SELECT getNamedScalar('cv_db');
SELECT current_value_is_valid, coalesce(exception, '') = '' AS no_error
FROM system.named_scalars WHERE name = 'cv_db';
" 2>&1

# Run 3: confirm the persisted .sql definition is self-contained.
SQL_FILE="$(ls ${TMP_DIR%/}/named_scalars/named_scalar_cv_db.sql 2>/dev/null | head -1)"
if [ -n "$SQL_FILE" ] && [ -f "$SQL_FILE" ]; then
    grep -q 'FROM mydb.t' "$SQL_FILE" && echo "definition_is_qualified=1" || echo "definition_is_qualified=0"
else
    echo "sql_file_missing"
fi

rm -rf "$TMP_DIR"
