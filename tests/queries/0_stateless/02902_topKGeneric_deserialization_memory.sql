-- https://github.com/ClickHouse/ClickHouse/issues/49706
-- The code under test runs in the client: it receives the `AggregateFunction` column over the
-- native protocol and deserializes the state. Reading a `topKResample` state must allocate only
-- what the state actually holds, never the `reserved` capacity implied by the parameters of
-- `topKResample` - that would be ~3M counters for each of the 6528 resample buckets, and the
-- client would run out of memory.
-- The output is dumped to `/dev/null` because the state itself is binary and uninteresting here.
-- `FORMAT Null` cannot be used instead: for it the server does not send the data to the client
-- at all (`null_format` in `executeQuery`), so nothing would be deserialized.
SELECT
    topKResampleState(1048576, 257, 65536, 10)(toString(number), number)
FROM numbers(3)
INTO OUTFILE '/dev/null' TRUNCATE FORMAT RowBinary;
