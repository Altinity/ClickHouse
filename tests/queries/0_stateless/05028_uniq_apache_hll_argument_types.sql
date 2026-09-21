-- Tags: no-fasttest
-- ^ DataSketches is not built in fast-test builds.

-- `uniqApacheHLL` accepts only the types a sketch hashes the same way in every implementation, as
-- an 8-byte integer, an IEEE-754 double or their raw bytes. Anything else would have to be hashed
-- to a single value by ClickHouse first, which no producer outside ClickHouse could reproduce.
-- All counts below are small enough for the sketch to stay in coupon mode, where it is exact.

SELECT 'types the sketch hashes directly';
SELECT uniqApacheHLL(toUInt64(number)) FROM numbers(20);
SELECT uniqApacheHLL(toInt32(number)) FROM numbers(20);
SELECT uniqApacheHLL(toBFloat16(number)) FROM numbers(20);
SELECT uniqApacheHLL(toFloat32(number)) FROM numbers(20);
SELECT uniqApacheHLL(toFloat64(number)) FROM numbers(20);
SELECT uniqApacheHLL(toString(number)) FROM numbers(20);
SELECT uniqApacheHLL(toFixedString(toString(number), 8)) FROM numbers(20);
SELECT uniqApacheHLL(toDate('2020-01-01') + number) FROM numbers(20);
SELECT uniqApacheHLL(toDate32('2020-01-01') + number) FROM numbers(20);
SELECT uniqApacheHLL(toDateTime('2020-01-01 00:00:00') + number) FROM numbers(20);
SELECT uniqApacheHLL(toDateTime64('2020-01-01 00:00:00.000', 3) + number) FROM numbers(20);
SELECT uniqApacheHLL(reinterpretAsUUID(toUInt128(number))) FROM numbers(20);
SELECT uniqApacheHLL(toIPv4('1.2.3.0') + number) FROM numbers(20);
SELECT uniqApacheHLL(toIPv6(concat('2001:db8::', hex(number + 1)))) FROM numbers(20);
SELECT uniqApacheHLL(CAST(number % 3, 'Enum8(\'a\' = 0, \'b\' = 1, \'c\' = 2)')) FROM numbers(20);

SELECT 'the wrappers of an accepted type are accepted';
SELECT uniqApacheHLL(toNullable(number)) FROM numbers(20);
SELECT uniqApacheHLL(toLowCardinality(toString(number))) FROM numbers(20);

SELECT 'a DateTime64 is the integer a caller elsewhere would hash';
-- `DateTime64(3)` counts epoch milliseconds, so it produces the sketch of those milliseconds.
SELECT hex(toString(uniqApacheHLLState(toDateTime64('2020-01-01 00:00:00.000', 3, 'UTC') + number)))
     = hex(toString(uniqApacheHLLState(toInt64(1577836800000) + number * 1000)))
FROM numbers(5) SETTINGS max_threads = 1;

SELECT 'types no other implementation can reproduce are rejected';
-- No byte order for the wide integers is agreed on across implementations.
SELECT uniqApacheHLL(toInt128(number)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL(toUInt256(number)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
-- No DataSketches binding accepts a decimal, and the scale is not part of the sketch.
SELECT uniqApacheHLL(toDecimal64(number, 2)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL(toDecimal128(number, 4)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
-- A composite value would have to be hashed by ClickHouse first.
SELECT uniqApacheHLL(materialize([number])) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL((number, number + 1)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }

SELECT 'one argument only';
SELECT uniqApacheHLL(number, number + 1) FROM numbers(20); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT uniqApacheHLL(toString(number), number) FROM numbers(20); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT uniqApacheHLL() FROM numbers(1); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }

SELECT 'the parameters still apply';
SELECT uniqApacheHLL(8)(reinterpretAsUUID(toUInt128(number))) FROM numbers(20);
SELECT toTypeName(uniqApacheHLLState(8, 'HLL_6')(number)) FROM numbers(1);
