-- Tags: no-fasttest, no-cpu-s390x
-- ^ DataSketches is not built in fast-test builds.

-- `lg_k` and the storage type of `uniqApacheHLL` are applied to every sketch that is merged into a state and written, so states
-- of different values of them are different types: a common type is taken without a `CAST` only for equal types, and then the
-- states are merged into the states of the first type, which would lower the resolution of a sketch. The estimator does not
-- change the state, and the omitted parameters are equal to their defaults, so these spellings are the same type.

SELECT 'the spellings of the same parameters are one type';
SELECT toTypeName([uniqApacheHLLState(toUInt64(1)), uniqApacheHLLState(12)(toUInt64(2)), uniqApacheHLLState(12, 'HLL_4')(toUInt64(3)), uniqApacheHLLState(12, 'HLL_4', 'COMPOSITE')(toUInt64(4))]) FROM numbers(1);

SELECT 'if between states of different estimators';
SELECT toTypeName(if(1, uniqApacheHLLState(8, 'HLL_6')(number), uniqApacheHLLState(8, 'HLL_6', 'COMPOSITE')(number))) FROM numbers(1);

SELECT 'a state with a Nullable argument is the same type as well';
SELECT toTypeName([uniqApacheHLLState(8)(toNullable(toUInt64(1))), uniqApacheHLLState(8)(toUInt64(2))]) FROM numbers(1);

SELECT 'a different lg_k is a different type';
SELECT toTypeName([uniqApacheHLLState(4)(toUInt64(1)), uniqApacheHLLState(14)(toUInt64(2))]) FROM numbers(1);

SELECT 'a different storage type is a different type';
SELECT toTypeName([uniqApacheHLLState(8, 'HLL_4')(toUInt64(1)), uniqApacheHLLState(8, 'HLL_8')(toUInt64(2))]) FROM numbers(1);

SELECT 'a different argument type is a different type';
SELECT toTypeName([uniqApacheHLLState(8)(toUInt64(1)), uniqApacheHLLState(8)(toString(2))]) FROM numbers(1);

SELECT 'UNION ALL of states with different parameters after a CAST';
SELECT uniqApacheHLLMerge(s) FROM
(
    SELECT CAST(uniqApacheHLLState(8)(toUInt64(number)), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s FROM numbers(3)
    UNION ALL
    SELECT CAST(uniqApacheHLLState(14, 'HLL_6')(toUInt64(number + 10)), 'AggregateFunction(uniqApacheHLL, UInt64)') AS s FROM numbers(3)
    UNION ALL
    SELECT uniqApacheHLLState(toUInt64(number + 20)) AS s FROM numbers(3)
);

SELECT 'a sketch of a higher lg_k keeps its resolution next to a state of a lower one';
WITH (SELECT uniqApacheHLLState(14, 'HLL_8')(number) FROM numbers(100000)) AS original
SELECT toString(variantElement(a[2], 'AggregateFunction(uniqApacheHLL(14, \'HLL_8\'), UInt64)')) = toString(original)
FROM (SELECT [(SELECT uniqApacheHLLState(4)(toUInt64(1))), original] AS a);
WITH (SELECT uniqApacheHLLState(14, 'HLL_8')(number) FROM numbers(100000)) AS original
SELECT toString(variantElement(s, 'AggregateFunction(uniqApacheHLL(14, \'HLL_8\'), UInt64)')) = toString(original)
FROM (SELECT if(c, (SELECT uniqApacheHLLState(4)(toUInt64(1))), original) AS s FROM (SELECT 0 AS c));

SELECT 'changing the parameters of a column is applied';
DROP TABLE IF EXISTS hll_state_types;
CREATE TABLE hll_state_types (k UInt8, s AggregateFunction(uniqApacheHLL, UInt64)) ENGINE = AggregatingMergeTree ORDER BY k;
INSERT INTO hll_state_types SELECT 1, uniqApacheHLLState(number) FROM numbers(5);
ALTER TABLE hll_state_types MODIFY COLUMN s AggregateFunction(uniqApacheHLL(8), UInt64);
SELECT toTypeName(s), uniqApacheHLLMerge(8)(s) FROM hll_state_types GROUP BY toTypeName(s);
DROP TABLE hll_state_types;
