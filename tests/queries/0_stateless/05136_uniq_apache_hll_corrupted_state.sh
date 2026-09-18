#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest -- compiled w/o datasketches

# A `uniqApacheHLL` state that is not a sketch must be rejected as `CORRUPTED_DATA`.
#
# `datasketches` reports a payload it cannot parse as `std::invalid_argument` or
# `std::out_of_range`, neither of which is a `DB::Exception`, so without the
# translation in `HllSketchData::read` they would escape
# `SerializationAggregateFunction`'s `catch (...)` and be reported as a logical error.
#
# This is a shell test rather than a `.sql` one with a `serverError` hint because the
# client prints an extra exception with a stack trace to stderr for this class of error,
# which a `.sql` test counts as a failure. The same applies to `uniqTheta`, see
# `04307_uniqTheta_corrupted_state_106259.sh`.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# A well-formed varint length prefix followed by eight bytes that are not a sketch:
# the first byte of the payload is the preamble length, which no HLL sketch uses.
$CLICKHOUSE_CLIENT --query \
    "SELECT finalizeAggregation(CAST(unhex('08FFFFFFFFFFFFFFFF'), 'AggregateFunction(uniqApacheHLL, UInt64)'))" 2>&1 \
    | grep -q -F 'CORRUPTED_DATA' && echo 'OK unknown type' || echo 'FAIL unknown type'

$CLICKHOUSE_CLIENT --query \
    "SELECT finalizeAggregation(CAST(unhex('0801020304050607FF'), 'AggregateFunction(uniqApacheHLL, UInt64)'))" 2>&1 \
    | grep -q -F 'CORRUPTED_DATA' && echo 'OK bad payload' || echo 'FAIL bad payload'

# The `RowBinary` path reaches `HllSketchData::read` through `deserializeBinary`:
# the leading 0x03 claims a three-byte payload, shorter than any HLL sketch.
printf '\x03\x03\x03\x30\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' \
    | $CLICKHOUSE_LOCAL --input-format=RowBinary \
        --structure='x AggregateFunction(uniqApacheHLL, IPv6)' \
        --query='SELECT x FROM table' 2>&1 \
    | grep -q -F 'CORRUPTED_DATA' && echo 'OK rowbinary' || echo 'FAIL rowbinary'
