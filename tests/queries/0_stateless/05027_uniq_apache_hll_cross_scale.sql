-- Tags: no-fasttest
-- ^ DataSketches is not built in fast-test builds.

-- `lg_k` and the sketch type configure the sketch but are held by the aggregate function rather than
-- by its state, whose layout is the same whatever they are, and a serialized sketch records its own
-- `lg_k`. `uniqApacheHLL` therefore reports states of different parameterisations as having one
-- binary representation, which lets `-Merge` read a state built with one `lg_k` under a function
-- declared with another, and lets `CAST` relabel a column between the two types.

SELECT 'the two types remain distinct';
SELECT toTypeName(uniqApacheHLLState(number)) FROM numbers(1);
SELECT toTypeName(uniqApacheHLLState(8)(number)) FROM numbers(1);

SELECT 'merging states built with a different lg_k';
-- Downsampling during a union produces the same registers as building at the target `lg_k` from the
-- start, so rescaling 17 states of `lg_k` 12 agrees exactly with merging 17 built at 8. Both sides
-- come out of a union and so use the same estimator.
SELECT
    (SELECT uniqApacheHLLMerge(8)(s) FROM (SELECT uniqApacheHLLState(number)    AS s FROM numbers(100000) GROUP BY number % 17))
  = (SELECT uniqApacheHLLMerge(8)(s) FROM (SELECT uniqApacheHLLState(8)(number) AS s FROM numbers(100000) GROUP BY number % 17));

-- Merging is lossy in one direction only: the union takes the resolution of its coarsest input, so a
-- rescaled state is much smaller than the states it was built from.
SELECT
    length(toString(uniqApacheHLLMergeState(8)(s))) < length(toString(uniqApacheHLLMergeState(s))) / 8
FROM (SELECT uniqApacheHLLState(number) AS s FROM numbers(100000) GROUP BY number % 17);

SELECT 'relabelling with CAST does not rescale';
-- `CAST` between the two types re-associates the column with the other function without touching its
-- data, so the sketch keeps the resolution it was built with and the estimate does not change.
SELECT
    finalizeAggregation(CAST(s, 'AggregateFunction(uniqApacheHLL(8), UInt64)')) = finalizeAggregation(s),
    length(toString(CAST(s, 'AggregateFunction(uniqApacheHLL(8), UInt64)'))) = length(toString(s))
FROM (SELECT uniqApacheHLLState(number) AS s FROM numbers(1000));

SELECT 'a state of one lg_k can be stored in a column declared with another';
DROP TABLE IF EXISTS hll_cross_scale;
CREATE TABLE hll_cross_scale (s AggregateFunction(uniqApacheHLL, UInt64)) ENGINE = MergeTree ORDER BY tuple();
INSERT INTO hll_cross_scale SELECT uniqApacheHLLState(8)(number) FROM numbers(20);
SELECT uniqApacheHLLMerge(s) FROM hll_cross_scale;
DROP TABLE hll_cross_scale;

SELECT 'only the parameters are interchangeable';
-- The argument types and the function itself must still match: `haveEqualArgumentTypes` and the name
-- comparison keep these apart even though the parameters no longer do.
SELECT CAST(uniqApacheHLLState(toString(number)), 'AggregateFunction(uniqApacheHLL, UInt64)') FROM numbers(10); -- { serverError CANNOT_CONVERT_TYPE }
SELECT CAST(uniqThetaState(number), 'AggregateFunction(uniqApacheHLL, UInt64)') FROM numbers(10); -- { serverError CANNOT_CONVERT_TYPE }
