#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$CUR_DIR"/../shell_config.sh

SUFFIX="${CLICKHOUSE_DATABASE}_${RANDOM}"
FILE="agg_state_${SUFFIX}.parquet"
NESTED="agg_state_nested_${SUFFIX}.parquet"
MIXED="agg_state_mixed_${SUFFIX}.parquet"
VERSIONED="agg_state_versioned_${SUFFIX}.parquet"
REFUSED="agg_state_refused_${SUFFIX}.parquet"
SIMPLE="agg_state_simple_${SUFFIX}.parquet"
UNKNOWN="agg_state_unknown_${SUFFIX}.parquet"
MATRIX="agg_state_matrix_${SUFFIX}.parquet"
RETYPED_SOURCE="agg_state_retyped_source_${SUFFIX}.parquet"
RETYPED="agg_state_retyped_${SUFFIX}.parquet"
CACHED="agg_state_cached_${SUFFIX}.parquet"
CACHED_SKIP="agg_state_cached_skip_${SUFFIX}.parquet"

cleanup()
{
    rm -f "${USER_FILES_PATH}/${FILE}" "${USER_FILES_PATH}/${NESTED}" "${USER_FILES_PATH}/${MIXED}" \
        "${USER_FILES_PATH}/${VERSIONED}" "${USER_FILES_PATH}/${REFUSED}" \
        "${USER_FILES_PATH}/${SIMPLE}" "${USER_FILES_PATH}/${UNKNOWN}" "${USER_FILES_PATH}/${MATRIX}" \
        "${USER_FILES_PATH}/${RETYPED_SOURCE}" "${USER_FILES_PATH}/${RETYPED}" \
        "${USER_FILES_PATH}/${CACHED}" "${USER_FILES_PATH}/${CACHED_SKIP}"
}
trap cleanup EXIT

STATES="--allow_experimental_aggregate_function_states_in_parquet=1"
SKIP="--input_format_parquet_skip_columns_with_unsupported_types_in_schema_inference=1"
NO_CACHE="--schema_inference_use_cache_for_file=0"

echo '-- without the setting writing a state is refused, as it was before Parquet supported states'
if ${CLICKHOUSE_CLIENT} --query "
    INSERT INTO FUNCTION file('${REFUSED}', Parquet) SELECT uniqState(number) AS u FROM numbers(10)
" 2>&1 | grep -q "allow_experimental_aggregate_function_states_in_parquet"
then
    echo 1
fi

echo '-- a SimpleAggregateFunction is an ordinary value of its storage type, so it needs no setting'
${CLICKHOUSE_CLIENT} --query "
    INSERT INTO FUNCTION file('${SIMPLE}', Parquet) SELECT sumSimpleState(number) AS s FROM numbers(10)
"
${CLICKHOUSE_CLIENT} --query "DESC file('${SIMPLE}', Parquet)"

${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${FILE}', Parquet)
    SELECT
        number % 3 AS k,
        uniqState(toUInt8(number % 17)) AS u,
        sumSimpleState(number) AS s
    FROM numbers(100)
    GROUP BY k
"

echo '-- without the setting the recorded state type is refused, not silently read as String'
if ${CLICKHOUSE_CLIENT} --query "DESC file('${FILE}', Parquet)" 2>&1 \
    | grep -q "allow_experimental_aggregate_function_states_in_parquet"
then
    echo 1
fi

echo '-- schema inference recovers the aggregate types from the file'
${CLICKHOUSE_CLIENT} ${STATES} --query "DESC file('${FILE}', Parquet)"

echo '-- states round-trip: merged per group'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    SELECT k, uniqMerge(u), sum(s) FROM file('${FILE}', Parquet) GROUP BY k ORDER BY k
"

echo '-- an explicit structure overrides the recorded type'
${CLICKHOUSE_CLIENT} --query "
    SELECT uniqMerge(u) FROM file('${FILE}', Parquet, 'u AggregateFunction(uniq, UInt8)')
"

echo '-- a column chunk holding both a dictionary page and a plain page'
# Exercise both dictionary and plain pages in one column chunk.
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${MIXED}', Parquet)
    SELECT uniqState(number) AS u FROM numbers(2000)
    GROUP BY number % 500
    SETTINGS output_format_parquet_max_dictionary_size = 20000, output_format_parquet_row_group_size = 100000
"
${CLICKHOUSE_CLIENT} --query "
    SELECT arraySort(tupleElement(arrayJoin(columns), 'encodings')) FROM file('${MIXED}', ParquetMetadata)
"
${CLICKHOUSE_CLIENT} ${STATES} --query "
    SELECT count(), uniqMerge(u) = (SELECT uniq(number) FROM numbers(2000)) AS matches
    FROM file('${MIXED}', Parquet)
"

echo '-- a state nested in an Array'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${NESTED}', Parquet)
    SELECT [uniqState(number), uniqState(number + 100)] AS a FROM numbers(10)
"
${CLICKHOUSE_CLIENT} ${STATES} --query "DESC file('${NESTED}', Parquet)"
${CLICKHOUSE_CLIENT} ${STATES} --query "SELECT arrayMap(x -> finalizeAggregation(x), a) FROM file('${NESTED}', Parquet)"

echo '-- a state pinned to a non-default version keeps that version'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${VERSIONED}', Parquet)
    SELECT CAST(sumMapState([number % 3], [toUInt32(number)]) AS AggregateFunction(0, sumMap, Array(UInt8), Array(UInt32))) AS m
    FROM numbers(100)
"
grep -ao 'AggregateFunction(0, sumMap[^"]*' "${USER_FILES_PATH}/${VERSIONED}"
${CLICKHOUSE_CLIENT} ${STATES} --query "
    SELECT sumMapMerge(m) = (SELECT sumMap([number % 3], [toUInt32(number)]) FROM numbers(100)) AS matches
    FROM file('${VERSIONED}', Parquet)
"

echo '-- a refused state column can be skipped instead of refusing the whole file'
${CLICKHOUSE_CLIENT} ${SKIP} ${NO_CACHE} --query "DESC file('${FILE}', Parquet)"
${CLICKHOUSE_CLIENT} ${SKIP} ${NO_CACHE} --query "SELECT sum(k) FROM file('${FILE}', Parquet)"

echo '-- an annotation naming an aggregate function this server does not have breaks only its column'
python3 -c "
import sys
data = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(data.replace(b'AggregateFunction(uniq,', b'AggregateFunction(zzzz,'))
" "${USER_FILES_PATH}/${FILE}" "${USER_FILES_PATH}/${UNKNOWN}"
if ${CLICKHOUSE_CLIENT} ${STATES} ${NO_CACHE} --query "DESC file('${UNKNOWN}', Parquet)" 2>&1 \
    | grep -q "for column u"
then
    echo 1
fi
${CLICKHOUSE_CLIENT} ${STATES} ${SKIP} ${NO_CACHE} --query "DESC file('${UNKNOWN}', Parquet)"
${CLICKHOUSE_CLIENT} ${STATES} ${SKIP} ${NO_CACHE} --query "SELECT sum(k) FROM file('${UNKNOWN}', Parquet)"

echo '-- every annotated type this writer can produce still reads back, values intact'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${MATRIX}', Parquet)
    SELECT
        sumSimpleState(number) AS num,
        anyLastSimpleState('abc') AS str,
        anyLastSimpleState(CAST('xyz', 'Nullable(String)')) AS nullable,
        CAST(anyLastSimpleState(toLowCardinality('lc')),
             'SimpleAggregateFunction(anyLast, LowCardinality(String))') AS low_cardinality,
        anyLastSimpleState([toUInt64(1), toUInt64(2)]) AS arr,
        anyLastSimpleState(map('k', toUInt64(7))) AS m,
        anyLastSimpleState(toDate('2020-01-02')) AS d,
        anyLastSimpleState(toDateTime('2020-01-02 03:04:05', 'UTC')) AS dt,
        anyLastSimpleState(toDateTime64('2020-01-02 03:04:05.1234', 4, 'UTC')) AS dt64,
        anyLastSimpleState(CAST('b', 'Enum8(''a'' = 1, ''b'' = 2)')) AS e,
        anyLastSimpleState(toIPv4('1.2.3.4')) AS ip4,
        anyLastSimpleState(toIPv6('::1')) AS ip6,
        anyLastSimpleState(toInt128('170141183460469231731687303715884105727')) AS i128,
        anyLastSimpleState(CAST('fixed', 'FixedString(16)')) AS fs,
        anyLastSimpleState(toUUID('00000000-0000-0000-0000-000000000001')) AS uu,
        anyLastSimpleState(toDecimal64('1.25', 4)) AS dec,
        uniqState(number) AS state,
        tuple(uniqState(number), toUInt64(7)) AS tup
    FROM numbers(3)
"
${CLICKHOUSE_CLIENT} ${STATES} --query "DESC file('${MATRIX}', Parquet)"
${CLICKHOUSE_CLIENT} ${STATES} --query "
    SELECT num, str, nullable, low_cardinality, arr, m, d, dt, dt64, e, ip4, ip6, toString(i128) AS i128, hex(fs), uu, dec,
           finalizeAggregation(state) AS uniq_state, finalizeAggregation(tup.1) AS uniq_in_tuple, tup.2 AS plain_in_tuple
    FROM file('${MATRIX}', Parquet)
    FORMAT Vertical
"

echo '-- the same file reads back with the nullability the reader is told to infer'
${CLICKHOUSE_CLIENT} ${STATES} ${NO_CACHE} --schema_inference_make_columns_nullable=1 --query "
    SELECT count() FROM file('${MATRIX}', Parquet)
"
${CLICKHOUSE_CLIENT} ${STATES} ${NO_CACHE} --schema_inference_make_columns_nullable=0 --query "
    SELECT count() FROM file('${MATRIX}', Parquet)
"

echo '-- a recorded type that re-reads the stored bytes as something else is refused'
${CLICKHOUSE_CLIENT} --query "
    INSERT INTO FUNCTION file('${RETYPED_SOURCE}', Parquet)
    SELECT toUInt8(number) AS k, sumWithOverflowSimpleState(toInt64(number)) AS v FROM numbers(3) GROUP BY k
"
python3 -c "
import sys
old = b'SimpleAggregateFunction(sumWithOverflow, Int64)'
new = b'SimpleAggregateFunction(anyLast, DateTime64(9))'
assert len(old) == len(new)
data = open(sys.argv[1], 'rb').read()
assert old in data
open(sys.argv[2], 'wb').write(data.replace(old, new))
" "${USER_FILES_PATH}/${RETYPED_SOURCE}" "${USER_FILES_PATH}/${RETYPED}"
if ${CLICKHOUSE_CLIENT} ${NO_CACHE} --query "DESC file('${RETYPED}', Parquet)" 2>&1 \
    | grep -q "reads as Int64"
then
    echo 1
fi
${CLICKHOUSE_CLIENT} ${SKIP} ${NO_CACHE} --query "SELECT sum(k) FROM file('${RETYPED}', Parquet)"

echo '-- a schema inferred with the setting on is not served to a query that has it off'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${CACHED}', Parquet)
    SELECT toUInt8(number % 3) AS k, uniqState(toUInt8(number)) AS u FROM numbers(30) GROUP BY k
"
# Ensure the file predates the cache entry despite one-second timestamp granularity.
touch -t 200001010000 "${USER_FILES_PATH}/${CACHED}"
${CLICKHOUSE_CLIENT} ${STATES} --query "DESC file('${CACHED}', Parquet)"
if ${CLICKHOUSE_CLIENT} --query "DESC file('${CACHED}', Parquet)" 2>&1 \
    | grep -q "allow_experimental_aggregate_function_states_in_parquet"
then
    echo 1
fi

echo '-- nor is a schema inferred with the refused column skipped served to a query that is not skipping'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${CACHED_SKIP}', Parquet)
    SELECT toUInt8(number % 3) AS k, uniqState(toUInt8(number)) AS u FROM numbers(30) GROUP BY k
"
touch -t 200001010000 "${USER_FILES_PATH}/${CACHED_SKIP}"
${CLICKHOUSE_CLIENT} ${SKIP} --query "DESC file('${CACHED_SKIP}', Parquet)"
if ${CLICKHOUSE_CLIENT} --query "DESC file('${CACHED_SKIP}', Parquet)" 2>&1 \
    | grep -q "allow_experimental_aggregate_function_states_in_parquet"
then
    echo 1
fi
