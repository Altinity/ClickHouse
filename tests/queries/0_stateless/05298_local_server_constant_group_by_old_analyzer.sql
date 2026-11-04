-- A local query has one server, so a folded server constant is an ordinary GROUP BY constant.
-- Keeping it makes `Dynamic` fail group-key validation.
-- https://github.com/Altinity/ClickHouse/pull/2490

SELECT count() FROM numbers(3) GROUP BY hostName() SETTINGS enable_analyzer = 0;

SELECT count() FROM numbers(3) GROUP BY CAST(hostName(), 'Dynamic') SETTINGS enable_analyzer = 0;

SELECT count() FROM numbers(3) GROUP BY [CAST(hostName(), 'Dynamic')] SETTINGS enable_analyzer = 0;
