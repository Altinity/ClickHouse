CREATE TEMPORARY TABLE args (value Array(Int)) ENGINE=Memory AS SELECT [1, 1, 1, 1] as value FROM numbers(1, 100);
SELECT length(arrayWithConstant(1000000, value)) FROM args FORMAT NULL;
