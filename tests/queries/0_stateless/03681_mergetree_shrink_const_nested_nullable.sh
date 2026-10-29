#!/usr/bin/env bash

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

# This test covers #90695: a constant inside PREWHERE, wrapped in materialize(toNullable(materialize(...))),
# creates a ColumnNullable whose top-level use_count() == 1 but whose *nested* column is shared with the
# query-wide ActionsDAG. The old `if (column->use_count() == 1) column->assumeMutableRef().shrinkToFit();`
# guard in MergeTreeReadTask::read() only checks the top-level refcount, so it still shrinks (realloc's)
# the shared nested column in place from multiple threads, causing a double-free / heap corruption.
#
# Fixture: 8 partitions of 1 row each, so ColumnConst::convertToFullColumn()'s `if (s == 1) return data;`
# shortcut hands out the shared one-element column instead of copying it.

setup() {
    $CLICKHOUSE_CLIENT -q "
        DROP TABLE IF EXISTS const_nested_nullable;
        CREATE TABLE const_nested_nullable (x Int16) ENGINE = MergeTree PARTITION BY x ORDER BY x;
        INSERT INTO const_nested_nullable VALUES (1), (2), (3), (4), (5), (6), (7), (8);
    "
}

run_queries() {
    # During 30 seconds we gonna hammer server with these SELECT queries. Before the fix, it'd crash with high probability. Not crashing is the expected success.
    local TIMELIMIT=$((SECONDS+30))
    while [ $SECONDS -lt "$TIMELIMIT" ]; do
        $CLICKHOUSE_CLIENT -q "
            SELECT median(3) IGNORE NULLS FROM const_nested_nullable PREWHERE and(materialize(toNullable(materialize(1))), not(materialize(100) = *)) FORMAT NULL;
        "
    done
}

cleanup() {
    $CLICKHOUSE_CLIENT -q "
        DROP TABLE IF EXISTS const_nested_nullable;
    "
}

setup
run_queries
cleanup
