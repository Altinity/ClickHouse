#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# `DESCRIBE` plans the query in full, so the planner checks `SELECT` on every column a nested subquery reads.
# A user who can read only `a` must not learn the type of `secret` through a subquery, as with `SELECT`.

U="u_05818_${CLICKHOUSE_DATABASE}"
$CLICKHOUSE_CLIENT -q "DROP USER IF EXISTS $U"
$CLICKHOUSE_CLIENT -q "CREATE TABLE t_05818 (a UInt8, secret String) ENGINE = Memory"
$CLICKHOUSE_CLIENT -q "CREATE USER $U"
$CLICKHOUSE_CLIENT -q "GRANT SELECT(a) ON ${CLICKHOUSE_DATABASE}.t_05818 TO $U"

run()
{
    local out
    out=$($CLICKHOUSE_CLIENT --user "$U" -q "$1" 2>&1)
    echo "$2"
    if grep -q 'ACCESS_DENIED' <<< "$out"; then
        echo 'ACCESS_DENIED'
    else
        echo "$out" | cut -f1,2
    fi
}

run "SELECT secret FROM ${CLICKHOUSE_DATABASE}.t_05818" 'select secret'
run "DESCRIBE (SELECT secret FROM ${CLICKHOUSE_DATABASE}.t_05818)" 'describe secret'
run "DESCRIBE (SELECT * FROM (SELECT secret FROM ${CLICKHOUSE_DATABASE}.t_05818))" 'describe nested secret'
run "DESCRIBE (WITH x AS (SELECT secret FROM ${CLICKHOUSE_DATABASE}.t_05818) SELECT * FROM x)" 'describe cte secret'
run "DESCRIBE (SELECT a FROM (SELECT a FROM ${CLICKHOUSE_DATABASE}.t_05818))" 'describe nested granted'

$CLICKHOUSE_CLIENT -q "DROP USER $U"
$CLICKHOUSE_CLIENT -q "DROP TABLE t_05818"
