#!/usr/bin/env bash
# Tags: no-parallel
#
# Refresh-task behavior for local named scalars. Converted from .sql to .sh
# because the asynchronous refresh task makes timing-based assertions
# (`SELECT sleep(N) FORMAT Null` then `SELECT FROM system.named_scalars`)
# brittle: under CI load the refresh may not have fired by the time the
# next SELECT runs. Each post-refresh assertion is now a retry poll with
# a 15-second budget. The expected output matches the original .sql
# version exactly.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh
CLICKHOUSE_CLIENT="$CLICKHOUSE_CLIENT --allow_experimental_named_scalars=1"
CLICKHOUSE_LOCAL="$CLICKHOUSE_LOCAL --allow_experimental_named_scalars=1"

# Poll a query until it returns the expected output (one or more lines,
# tab-separated columns). Times out after 15 seconds; on timeout, prints
# whatever the last attempt returned so the diff vs reference is informative.
wait_for() {
    local expected="$1"
    local query="$2"
    local deadline=$((SECONDS + 15))
    local actual=""
    while [ "$SECONDS" -lt "$deadline" ]; do
        actual=$(${CLICKHOUSE_CLIENT} -q "$query" 2>/dev/null)
        if [ "$actual" = "$expected" ]; then
            echo "$expected"
            return 0
        fi
        sleep 0.25
    done
    echo "$actual"
    return 1
}

# -------- setup --------
${CLICKHOUSE_CLIENT} -m -q "
DROP NAMED SCALAR IF EXISTS cv_refresh;
DROP NAMED SCALAR IF EXISTS cv_refresh_fail;
DROP NAMED SCALAR IF EXISTS cv_refresh_const;
DROP NAMED SCALAR IF EXISTS cv_refresh_table_const;
DROP NAMED SCALAR IF EXISTS cv_flap;
DROP TABLE IF EXISTS default.cv_refresh_src;
DROP TABLE IF EXISTS default.cv_refresh_flags;
DROP TABLE IF EXISTS default.cv_flap_src;
"

# -------- cv_refresh: simplest refreshable scalar --------
${CLICKHOUSE_CLIENT} -m -q "
CREATE NAMED SCALAR cv_refresh REFRESH EVERY 1 SECOND AS SELECT now();
SYSTEM REFRESH NAMED SCALAR cv_refresh;
"
wait_for "local	cv_refresh	1	1" "
SELECT kind, name, has_value, current_value_is_valid
FROM system.named_scalars
WHERE kind = 'local' AND name = 'cv_refresh'
ORDER BY name
"

# -------- cv_refresh_fail: backing table goes away mid-refresh --------
${CLICKHOUSE_CLIENT} -m -q "
CREATE TABLE default.cv_refresh_src (x UInt8) ENGINE = Memory;
INSERT INTO default.cv_refresh_src VALUES (1);
CREATE NAMED SCALAR cv_refresh_fail REFRESH EVERY 1 SECOND AS (SELECT count() FROM default.cv_refresh_src);
SYSTEM REFRESH NAMED SCALAR cv_refresh_fail;
DROP TABLE default.cv_refresh_src;
SYSTEM REFRESH NAMED SCALAR cv_refresh_fail;
"
wait_for "local	cv_refresh_fail	1	0	1" "
SELECT kind, name, has_value, current_value_is_valid, coalesce(exception, '') != '' AS has_error
FROM system.named_scalars
WHERE kind = 'local' AND name = 'cv_refresh_fail'
ORDER BY name
"

# -------- cv_refresh_const + non-existent SYSTEM commands --------
${CLICKHOUSE_CLIENT} -q "CREATE NAMED SCALAR cv_refresh_const REFRESH EVERY 1 SECOND AS SELECT 1"
${CLICKHOUSE_CLIENT} -q "SELECT getNamedScalar('cv_refresh_const')"
${CLICKHOUSE_CLIENT} -q "SYSTEM STOP NAMED SCALAR REFRESHES cv_missing" 2>/dev/null
${CLICKHOUSE_CLIENT} -q "SYSTEM START NAMED SCALAR REFRESHES cv_missing" 2>/dev/null

# -------- cv_refresh_table_const --------
${CLICKHOUSE_CLIENT} -m -q "
CREATE TABLE default.cv_refresh_flags (enabled UInt8) ENGINE = Memory;
INSERT INTO default.cv_refresh_flags VALUES (1);
CREATE NAMED SCALAR cv_refresh_table_const REFRESH EVERY 1 SECOND AS SELECT 1 FROM default.cv_refresh_flags WHERE enabled LIMIT 1;
"
${CLICKHOUSE_CLIENT} -q "SELECT getNamedScalar('cv_refresh_table_const')"

# CREATE validates the expression synchronously on the initiator, so a missing
# backing relation fails the CREATE itself (the entry is not registered).
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS default.cv_missing_src"
${CLICKHOUSE_CLIENT} -q "CREATE NAMED SCALAR cv_broken REFRESH EVERY 36500 DAYS AS (SELECT count() FROM default.cv_missing_src)" 2>/dev/null

# -------- cv_flap: backing table flaps; last-good is preserved --------
${CLICKHOUSE_CLIENT} -m -q "
CREATE TABLE default.cv_flap_src (x UInt8) ENGINE = Memory;
INSERT INTO default.cv_flap_src VALUES (7);
CREATE NAMED SCALAR cv_flap REFRESH EVERY 36500 DAYS AS (SELECT count() FROM default.cv_flap_src);
"
${CLICKHOUSE_CLIENT} -q "SELECT getNamedScalar('cv_flap')"

# Drop the backing table; force a refresh; the scalar enters a stale-with-error state
# while keeping the previous good value visible.
${CLICKHOUSE_CLIENT} -m -q "
DROP TABLE default.cv_flap_src;
SYSTEM REFRESH NAMED SCALAR cv_flap;
"
wait_for "1	0	1	1" "
SELECT has_value, current_value_is_valid, coalesce(exception, '') != '' AS has_error,
       last_success_time > toDateTime('1970-01-01 00:00:00', 'UTC') AS kept_prior_success
FROM system.named_scalars
WHERE kind = 'local' AND name = 'cv_flap'
"

# During the outage, getNamedScalar still serves the last-good value.
${CLICKHOUSE_CLIENT} -q "SELECT getNamedScalar('cv_flap')"

# Recovery: re-create the backing table, force a refresh, current_value_is_valid
# returns to 1 and last_error is cleared.
${CLICKHOUSE_CLIENT} -m -q "
CREATE TABLE default.cv_flap_src (x UInt8) ENGINE = Memory;
INSERT INTO default.cv_flap_src VALUES (1), (2);
SYSTEM REFRESH NAMED SCALAR cv_flap;
"
wait_for "1	1	1" "
SELECT has_value, current_value_is_valid, coalesce(exception, '') = '' AS cleared_error
FROM system.named_scalars
WHERE kind = 'local' AND name = 'cv_flap'
"
${CLICKHOUSE_CLIENT} -q "SELECT getNamedScalar('cv_flap')"

# -------- cleanup --------
${CLICKHOUSE_CLIENT} -m -q "
DROP NAMED SCALAR cv_refresh;
DROP NAMED SCALAR cv_refresh_fail;
DROP NAMED SCALAR cv_refresh_const;
DROP NAMED SCALAR cv_refresh_table_const;
DROP NAMED SCALAR cv_flap;
DROP TABLE IF EXISTS default.cv_refresh_src;
DROP TABLE IF EXISTS default.cv_refresh_flags;
DROP TABLE IF EXISTS default.cv_flap_src;
"
