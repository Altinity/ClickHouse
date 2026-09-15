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
REFUSED_NESTED="agg_state_refused_nested_${SUFFIX}.parquet"
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
        "${USER_FILES_PATH}/${REFUSED_NESTED}" "${USER_FILES_PATH}/${SIMPLE}" \
        "${USER_FILES_PATH}/${UNKNOWN}" "${USER_FILES_PATH}/${MATRIX}" \
        "${USER_FILES_PATH}/${RETYPED_SOURCE}" "${USER_FILES_PATH}/${RETYPED}" \
        "${USER_FILES_PATH}/${CACHED}" "${USER_FILES_PATH}/${CACHED_SKIP}"
}
trap cleanup EXIT

# The serialized states, and the type names recorded next to them, are a ClickHouse-only convention,
# and an `AggregateFunction` state is deserialized by whichever aggregate function the file's own
# metadata names, so both writing states and inferring them from a file are opt-in.
STATES="--allow_experimental_aggregate_function_states_in_parquet=1"
# A recorded type this server will not honour concerns one column, so it is skippable like a column
# of any other unsupported type.
SKIP="--input_format_parquet_skip_columns_with_unsupported_types_in_schema_inference=1"
# Several blocks below infer the same file again under a different value of the settings above. Those
# settings are part of the schema cache key, so the schema would be inferred afresh anyway; disabling
# the cache keeps those blocks independent of cache behaviour altogether. The two blocks at the end,
# which are about the cache key itself, deliberately do not use this.
NO_CACHE="--schema_inference_use_cache_for_file=0"

echo '-- without the setting writing a state is refused, as it was before Parquet supported states'
if ${CLICKHOUSE_CLIENT} --query "
    INSERT INTO FUNCTION file('${REFUSED}', Parquet) SELECT uniqState(number) AS u FROM numbers(10)
" 2>&1 | grep -q "allow_experimental_aggregate_function_states_in_parquet"
then
    echo 1
fi

echo '-- a state nested in an Array is refused just the same, the recursion reaching the leaf first'
if ${CLICKHOUSE_CLIENT} --query "
    INSERT INTO FUNCTION file('${REFUSED_NESTED}', Parquet) SELECT [uniqState(number)] AS a FROM numbers(10)
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
# Whether the server forwards its own log record for the exception depends on the logger
# configuration, so the setting name can be printed more than once - assert that it is named at all
# instead of how many lines name it.
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

echo '-- merging across all groups matches the source too'
${CLICKHOUSE_CLIENT} ${STATES} --query "
    SELECT uniqMerge(u) = (SELECT uniq(toUInt8(number % 17)) FROM numbers(100)) AS uniq_matches,
           sum(s) = (SELECT sum(number) FROM numbers(100)) AS sum_matches
    FROM file('${FILE}', Parquet)
"

echo '-- an explicit structure overrides the recorded type - and needs no setting, being the query'\''s own'
${CLICKHOUSE_CLIENT} --query "
    SELECT uniqMerge(CAST(u AS AggregateFunction(uniq, UInt8)))
    FROM file('${FILE}', Parquet, 'u String')
"
${CLICKHOUSE_CLIENT} --query "
    SELECT uniqMerge(u) FROM file('${FILE}', Parquet, 'u AggregateFunction(uniq, UInt8)')
"

echo '-- a column chunk holding both a dictionary page and a plain page'
# A dictionary budget that fits only some of the states makes the writer emit a dictionary page and
# then fall back to plain within the same column chunk, so the reader mixes Dictionary::index(),
# which shares state ownership via ColumnAggregateFunction::src, with the state converter, which
# allocates its own states. The encodings are asserted so this keeps covering the mixed case.
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
# `sumMap` is versioned, and version 0 serializes the values as they are while version 1 promotes
# them to a wider type, so reading a version-0 state as the default version 1 fails outright.
# getName() drops the pinned 0, so the annotation is the only place that can carry it.
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
# The schema is inferred as a whole, before the query prunes the columns it does not read, so by
# default one refused annotation still refuses a query that never touches that column. Skipping the
# column is the way out that does not enable the state deserializer: the rest of the file reads.
${CLICKHOUSE_CLIENT} ${SKIP} ${NO_CACHE} --query "DESC file('${FILE}', Parquet)"
${CLICKHOUSE_CLIENT} ${SKIP} ${NO_CACHE} --query "SELECT sum(k) FROM file('${FILE}', Parquet)"

echo '-- an annotation naming an aggregate function this server does not have breaks only its column'
# Such a file is what a newer ClickHouse, or a corrupted footer, produces. It is made here by
# overwriting the recorded function name in place with one no build has - the replacement is the same
# length, so every offset in the file, the thrift footer included, stays valid.
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
# The recorded type is matched strictly against the parquet schema, and parquet does not round-trip
# every ClickHouse type: `Date` becomes a DATE that reads back as `Date32`, `DateTime` a
# TIMESTAMP_MILLIS that reads back as `DateTime64(3)`, `Enum8` an ENUM byte array that reads back as
# `String`, `IPv4` a plain UINT_32, `IPv6` and `Int128` untyped fixed byte arrays, `LowCardinality(T)` a
# plain `T`, and any leaf may gain or lose a `Nullable`. So the matching has to allow exactly the
# mappings the writer performs - which is what this block pins down, by going through the real writer.
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
# `schema_inference_make_columns_nullable` decides whether a leaf derives as Nullable, and the
# recorded type says nothing about it, so neither setting may refuse the file.
${CLICKHOUSE_CLIENT} ${STATES} ${NO_CACHE} --schema_inference_make_columns_nullable=1 --query "
    SELECT count() FROM file('${MATRIX}', Parquet)
"
${CLICKHOUSE_CLIENT} ${STATES} ${NO_CACHE} --schema_inference_make_columns_nullable=0 --query "
    SELECT count() FROM file('${MATRIX}', Parquet)
"

echo '-- a recorded type that re-reads the stored bytes as something else is refused'
# `SimpleAggregateFunction` is honoured without a setting, so a stale or hand-crafted annotation is
# the one thing standing between an INT64 column and being read as nanosecond timestamps - castColumn
# converts the integers happily and silently, and the values a SELECT returns are then not the values
# in the file. The recorded name is overwritten in place with one of exactly the same length, so every
# offset in the file, the thrift footer included, stays valid.
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
# The inferred schema outlives the query that filled the cache, so the cache key has to name the
# settings that pick the schema. Otherwise a single query with the gate open would hand every later
# query, of any user, a schema carrying `AggregateFunction`, running the state deserializer the file
# names with the gate closed. Unlike the blocks above this one needs the cache, so it must not
# disable it, and it uses a file no earlier block has inferred under other settings.
${CLICKHOUSE_CLIENT} ${STATES} --query "
    INSERT INTO FUNCTION file('${CACHED}', Parquet)
    SELECT toUInt8(number % 3) AS k, uniqState(toUInt8(number)) AS u FROM numbers(30) GROUP BY k
"
# An entry is invalidated when the file is at least as new as the entry itself, and both timestamps
# have one-second granularity, so a file written and inferred within the same second is never served
# from the cache. Dating the file back makes the entry usable without waiting for a second to pass.
touch -t 200001010000 "${USER_FILES_PATH}/${CACHED}"
${CLICKHOUSE_CLIENT} ${STATES} --query "DESC file('${CACHED}', Parquet)"
if ${CLICKHOUSE_CLIENT} --query "DESC file('${CACHED}', Parquet)" 2>&1 \
    | grep -q "allow_experimental_aggregate_function_states_in_parquet"
then
    echo 1
fi

echo '-- nor is a schema inferred with the refused column skipped served to a query that is not skipping'
# The skip setting picks the schema too: with it the refused column is dropped from the schema, and
# without it inference throws instead, so neither answer may be cached for the other.
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
