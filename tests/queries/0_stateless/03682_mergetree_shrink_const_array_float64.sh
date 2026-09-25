#!/usr/bin/env bash

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

# This test covers a variant of #90695/#92588 via ColumnArray instead of ColumnNullable: arrayMap's lambda
# body evaluates a bare Float64 literal, which the analyzer treats as a per-block constant. When the array
# has a single element, ColumnConst::convertToFullColumn()'s `if (s == 1) return data;` shortcut hands out
# a shared literal column as the arrayMap result's *nested* data column, wrapped in a freshly-built
# ColumnArray whose top-level use_count() == 1. The old
# `if (column->use_count() == 1) column->assumeMutableRef().shrinkToFit();` guard in
# MergeTreeReadTask::read() only checks that top-level refcount, so ColumnArray::shrinkToFit() still
# shrinks (realloc's) the shared nested ColumnVector<Float64> in place from multiple threads, causing a
# double-free / heap corruption.
#
# Fixture: 8 partitions of 1 row each, so ColumnConst::convertToFullColumn()'s `if (s == 1) return data;`
# shortcut hands out the shared one-element column instead of copying it.

setup() {
    $CLICKHOUSE_CLIENT -q "
        DROP TABLE IF EXISTS const_array_float64;
        CREATE TABLE const_array_float64 (x Int16) ENGINE = MergeTree PARTITION BY x ORDER BY x;
        INSERT INTO const_array_float64 VALUES (1), (2), (3), (4), (5), (6), (7), (8);
    "
}

run_queries() {
    # During 30 seconds we gonna hammer server with these SELECT queries. Before the fix, it'd crash with high probability. Not crashing is the expected success.
    local TIMELIMIT=$((SECONDS+30))
    while [ $SECONDS -lt "$TIMELIMIT" ]; do
        $CLICKHOUSE_CLIENT -q "
            SELECT arrayMap(v -> 1., [x]) FROM const_array_float64 PREWHERE and(materialize(toNullable(materialize(1))), not(materialize(100) = x)) FORMAT NULL;
        "
    done
}

cleanup() {
    $CLICKHOUSE_CLIENT -q "
        DROP TABLE IF EXISTS const_array_float64;
    "
}

setup
run_queries
cleanup
