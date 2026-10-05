-- Tags: no-fasttest
-- - no-fasttest -- compiled w/o datasketches

-- Compressed Theta states (serialization version 4) exercise separate 33- and 35-bit
-- unpacking routines that previously underestimated cardinality. Each retains 16 values
-- with theta = 1; two shared values give a union of 30.

WITH
    CAST(unhex('4B01040321011ACC9310000001F4000000FA0000007D0000003E8000001F4000000FA0000007D180000000000001F4000000FA0000007D0000003E8000001F4000000FA0000007D0000003E8') AS AggregateFunction(uniqTheta, UInt64)) AS state_33_bits,
    CAST(unhex('4F01040323011ACC93100000007D0000000FA1000000004000000000000007D0000000FA0000001F40000003E80000007D0000000FA1000000000000003E80000007D0000000FA0000001F40000003E8') AS AggregateFunction(uniqTheta, UInt64)) AS state_35_bits
SELECT
    finalizeAggregation(state_33_bits),
    finalizeAggregation(state_35_bits),
    finalizeAggregation(uniqThetaUnion(state_33_bits, state_35_bits));
