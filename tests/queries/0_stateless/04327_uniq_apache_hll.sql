-- Tags: no-fasttest
-- ^ DataSketches is not built in fast-test builds.

SELECT 'accuracy';
-- HLL is approximate but deterministic; assert the estimate is within the expected error band.
SELECT abs(toInt64(uniqApacheHLL(number)) - 1000) < 30 FROM numbers(1000);
SELECT abs(toInt64(uniqApacheHLL(number)) - 100000) < 3000 FROM numbers(100000);
-- Higher lg_k -> better accuracy.
SELECT abs(toInt64(uniqApacheHLL(14)(number)) - 100000) < 1500 FROM numbers(100000);
-- Storage type does not change the estimate.
SELECT uniqApacheHLL(12, 'HLL_4')(number) = uniqApacheHLL(12, 'HLL_8')(number) FROM numbers(1000);

SELECT 'empty and single';
SELECT uniqApacheHLL(number) FROM numbers(0);
SELECT uniqApacheHLL(number) FROM numbers(1);

SELECT 'argument types';
SELECT abs(toInt64(uniqApacheHLL(toInt32(number))) - 500) < 20 FROM numbers(500);
SELECT abs(toInt64(uniqApacheHLL(toFloat64(number))) - 500) < 20 FROM numbers(500);
SELECT abs(toInt64(uniqApacheHLL(toString(number))) - 500) < 20 FROM numbers(500);
SELECT abs(toInt64(uniqApacheHLL(toDate('2020-01-01') + number)) - 500) < 20 FROM numbers(500);

SELECT 'state/merge roundtrip is native';
-- Merging per-group `-State` values covers exactly the same set as a direct aggregate, so the two
-- estimates agree to within the error of the sketch. They are not required to be equal: DataSketches
-- reports the HIP estimate for a sketch that has only been updated and the composite estimate for
-- one that came out of a union, so a merged result is not the same number as a directly built one
-- even though both are computed from identical registers.
SELECT
    abs(toInt64(uniqApacheHLLMerge(s)) - toInt64((SELECT uniqApacheHLL(number) FROM numbers(100000)))) < 3000
FROM
(
    SELECT uniqApacheHLLState(number) AS s
    FROM numbers(100000)
    GROUP BY number % 17
);

-- Merging is independent of how the input was partitioned: the registers of the union do not depend
-- on the grouping, and both sides come out of a union, so two different partitionings of the same
-- set agree exactly.
SELECT
    (SELECT uniqApacheHLLMerge(s) FROM (SELECT uniqApacheHLLState(number) AS s FROM numbers(100000) GROUP BY number % 17))
  = (SELECT uniqApacheHLLMerge(s) FROM (SELECT uniqApacheHLLState(number) AS s FROM numbers(100000) GROUP BY number % 13));

-- The same holds for a single state.
SELECT
    abs(toInt64(uniqApacheHLLMerge(s)) - toInt64((SELECT uniqApacheHLL(number) FROM numbers(1000)))) < 30
FROM
(
    SELECT uniqApacheHLLState(number) AS s FROM numbers(1000)
);

-- The state type carries the sketch parameters.
SELECT toTypeName(uniqApacheHLLState(14, 'HLL_8')(number)) FROM numbers(1);

SELECT 'parameter validation';
SELECT uniqApacheHLL(3)(number) FROM numbers(1); -- { serverError ARGUMENT_OUT_OF_BOUND }
SELECT uniqApacheHLL(22)(number) FROM numbers(1); -- { serverError ARGUMENT_OUT_OF_BOUND }
SELECT uniqApacheHLL(12, 'HLL_9')(number) FROM numbers(1); -- { serverError BAD_ARGUMENTS }
SELECT uniqApacheHLL(12, 'HLL_4', 1)(number) FROM numbers(1); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
