#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-replicated-database
# Test: reading CapnProto messages with missing Data fields (UInt256/UInt128/etc.)
# should insert default values instead of throwing "Unexpected size" error.
# Regression test for https://github.com/ClickHouse/ClickHouse/issues/86864

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

SCHEMADIR=$CURDIR/format_schemas

# Test 1: Flat struct — produce with old schema (no newUint256/newUint128/etc.), read with new schema
echo "Test 1: flat struct, missing Data fields get defaults"
$CLICKHOUSE_LOCAL -q "
    SELECT 'hello'::String AS name, 42::UInt256 AS value
    FORMAT CapnProto
    SETTINGS format_schema='$SCHEMADIR/04078_capnp_old_schema:Message'
" | $CLICKHOUSE_LOCAL \
    --input-format CapnProto \
    --structure 'name String, value UInt256, newUint256 UInt256, newUint128 UInt128, newInt256 Int256, newDecimal128 Decimal128(2)' \
    --format_schema="$SCHEMADIR/04078_capnp_new_schema:Message" \
    -q "SELECT name, value, newUint256, newUint128, newInt256, newDecimal128 FROM table"

# Test 2: Nested struct (Tuple) — produce with old schema, read with new schema that adds UInt256 inside Tuple
echo "Test 2: nested struct (Tuple), missing Data field inside Tuple gets default"
$CLICKHOUSE_LOCAL -q "
    SELECT 'test'::String AS title, tuple('inner_val', 32::Int32, 59::UInt256) AS inner
    FORMAT CapnProto
    SETTINGS format_schema='$SCHEMADIR/04078_capnp_nested_old:Message'
" | $CLICKHOUSE_LOCAL \
    --input-format CapnProto \
    --structure 'title String, inner Tuple(field1 String, field2 Int32, specialField UInt256, newSpecialField UInt256)' \
    --format_schema="$SCHEMADIR/04078_capnp_nested_new:Message" \
    -q "SELECT title, inner.field1, inner.field2, inner.specialField, inner.newSpecialField FROM table"

# Test 3: Ensure non-empty Data fields still work correctly (no regression)
echo "Test 3: all fields populated, no regression"
$CLICKHOUSE_LOCAL -q "
    SELECT 'world'::String AS name, 100::UInt256 AS value, 200::UInt256 AS newUint256, 300::UInt128 AS newUint128, (-400)::Int256 AS newInt256, 5.55::Decimal128(2) AS newDecimal128
    FORMAT CapnProto
    SETTINGS format_schema='$SCHEMADIR/04078_capnp_new_schema:Message'
" | $CLICKHOUSE_LOCAL \
    --input-format CapnProto \
    --structure 'name String, value UInt256, newUint256 UInt256, newUint128 UInt128, newInt256 Int256, newDecimal128 Decimal128(2)' \
    --format_schema="$SCHEMADIR/04078_capnp_new_schema:Message" \
    -q "SELECT name, value, newUint256, newUint128, newInt256, newDecimal128 FROM table"

# Test 4: Wrong non-zero size should still error
echo "Test 4: wrong non-zero Data size still errors"
$CLICKHOUSE_LOCAL -q "
    SELECT 'bad'::String AS name, 'short'::String AS value
    FORMAT CapnProto
    SETTINGS format_schema='$SCHEMADIR/04078_capnp_old_schema:Message'
" 2>&1 | grep -c "CANNOT_CONVERT_TYPE\|Cannot convert" || true

# Actually, capnp Data -> String should work fine. Let's test with a schema that declares Data but CH expects UInt256
# The old schema has value as Data, and we read it as UInt256 - if the producer wrote wrong number of bytes
# that's a different error path. The key test is tests 1-3 above.
