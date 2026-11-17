#!/usr/bin/env bash
# Tags: long

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -e

# Fixed-width arguments cover every specialized copy size, wider fields, null maps, constants and
# low-cardinality normalization. Block boundaries leave partial staging batches. Variable-width keys
# and arguments exercise the same fixed-field packing within records that also contain serialized data.
# The numeric keys are `UInt64`: a key of at most two bytes has no two-level form, so the adaptive
# aggregator would not engage. Both runs of a cell share a clickhouse-local process, the adaptive one
# first, so the counters in `system.events` belong to it alone: the run with the feature off adds none
# of them. Every cell reports whether the adaptive run froze its tables, staged records and spilled them.
aggregates="uniqUpTo(25)(number), sum(toUInt128(number)), sum(toDecimal256(number, 3)),
    min(toLowCardinality(toFixedString(char(65 + number % 26), 3))), min(7),
    min(toNullable(toUInt128(23))), max(toString(number)), max([number]),
    sequenceMatch('(?1)(?t<10000)(?2)')(toDateTime(number), number % 3 = 0, number % 5 = 0)"
for width in {1..32} 33 65
do
    value="toFixedString(char(65 + number % 26), ${width})"
    aggregates+=", min(${value}), min(if(number % 3 = 0, NULL, ${value}))"
done

check()
{
    local label=$1 key=$2 spill=$3
    local query="SELECT sum(cityHash64(*)) FROM (SELECT ${key} AS k, ${aggregates} FROM numbers_mt(40123) GROUP BY k)"
    $CLICKHOUSE_LOCAL --query "
        SET max_threads = 4, max_threads_min_free_memory_per_thread = 0, max_block_size = 8191;
        SET optimize_injective_functions_in_group_by = 0;
        SET adaptive_aggregator_freeze_threshold = 32, adaptive_aggregator_freeze_threshold_bytes = 0;
        SET adaptive_aggregator_disable_thaw = 1, collect_hash_table_stats_during_aggregation = 0;
        SET max_bytes_before_external_group_by = ${spill}, max_bytes_ratio_before_external_group_by = 0;
        ${query} SETTINGS enable_adaptive_aggregator = 1 INTO OUTFILE '${CLICKHOUSE_TMP}/argument_gather_1.out' TRUNCATE;
        ${query} SETTINGS enable_adaptive_aggregator = 0 INTO OUTFILE '${CLICKHOUSE_TMP}/argument_gather_0.out' TRUNCATE;
        SELECT '${label}', sumIf(value, event = 'AdaptiveAggregationLocalFreezes') > 0,
            sumIf(value, event = 'AdaptiveAggregationStagedRecords') > 0,
            sumIf(value, event = 'AdaptiveAggregationSpilledRecords') > 0 FROM system.events;
    "
    diff -u "${CLICKHOUSE_TMP}/argument_gather_0.out" "${CLICKHOUSE_TMP}/argument_gather_1.out"
}

check 'Numeric key' 'toUInt64(number % 5000)' 0
check 'Packed keys' 'tuple(number % 5000, toUInt32(number % 7))' 0
check 'String key' "concat('key_', toString(number % 5000))" 0
check 'Serialized key' '[number % 5000]' 0
check 'Spilled arguments' 'toUInt64(number % 5000)' 1000000

# Successive fixed records exercise key and argument copies at different alignments within a chunk.
for width in 4 5 6 7
do
    aggregates="min(toFixedString(char(65 + number % 26), ${width}))"
    check "Fixed record width ${width}" 'toUInt64(number % 5000)' 0
done
