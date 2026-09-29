-- Tags: no-fasttest
-- ^ DataSketches is not built in fast-test builds.

SELECT 'accuracy';
SELECT abs(toInt64(uniqApacheHLL(number)) - 100000) < 3000 FROM numbers(100000);
SELECT abs(toInt64(uniqApacheHLL(14)(number)) - 100000) < 1500 FROM numbers(100000);
-- Storage type does not change the estimate.
SELECT
    uniqApacheHLL(12, 'HLL_4')(number) = uniqApacheHLL(12, 'HLL_6')(number),
    uniqApacheHLL(12, 'HLL_4')(number) = uniqApacheHLL(12, 'HLL_8')(number)
FROM numbers(1000);

SELECT 'empty and single';
SELECT uniqApacheHLL(number) FROM numbers(0);
SELECT uniqApacheHLL(number) FROM numbers(1);

SELECT 'state and merge';
-- Direct and merged sketches may use different estimators.
SELECT
    abs(toInt64(uniqApacheHLLMerge(s)) - toInt64((SELECT uniqApacheHLL(number) FROM numbers(100000)))) < 3000
FROM
(
    SELECT uniqApacheHLLState(number) AS s
    FROM numbers(100000)
    GROUP BY number % 17
);

-- Different partitions must produce the same union estimate.
SELECT
    (SELECT uniqApacheHLLMerge(s) FROM (SELECT uniqApacheHLLState(number) AS s FROM numbers(100000) GROUP BY number % 17))
  = (SELECT uniqApacheHLLMerge(s) FROM (SELECT uniqApacheHLLState(number) AS s FROM numbers(100000) GROUP BY number % 13));

SELECT toTypeName(uniqApacheHLLState(14, 'HLL_8')(number)) FROM numbers(1);

SELECT 'parameter validation';
SELECT uniqApacheHLL(3)(number) FROM numbers(1); -- { serverError ARGUMENT_OUT_OF_BOUND }
SELECT uniqApacheHLL(22)(number) FROM numbers(1); -- { serverError ARGUMENT_OUT_OF_BOUND }
SELECT uniqApacheHLL(12, 'HLL_9')(number) FROM numbers(1); -- { serverError BAD_ARGUMENTS }
SELECT uniqApacheHLL(12, 'HLL_4', 1)(number) FROM numbers(1); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }

SELECT 'argument types';
-- Small counts stay in coupon mode, where the estimate is exact.
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
SELECT uniqApacheHLL(toIPv4('1.2.3.0') + number) FROM numbers(20);
SELECT uniqApacheHLL(CAST(number % 3, 'Enum8(\'a\' = 0, \'b\' = 1, \'c\' = 2)')) FROM numbers(20);

SELECT 'nullable and low cardinality';
SELECT uniqApacheHLL(toNullable(number)) FROM numbers(20);
SELECT uniqApacheHLL(toLowCardinality(toString(number))) FROM numbers(20);

SELECT 'unsupported types';
SELECT uniqApacheHLL(toInt128(number)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL(toUInt256(number)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL(toDecimal64(number, 2)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL(materialize([number])) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT uniqApacheHLL((number, number + 1)) FROM numbers(20); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }

SELECT 'one argument only';
SELECT uniqApacheHLL(number, number + 1) FROM numbers(20); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT uniqApacheHLL() FROM numbers(1); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
