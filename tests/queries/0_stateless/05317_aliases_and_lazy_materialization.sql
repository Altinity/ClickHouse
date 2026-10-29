-- Tags: no-random-settings, no-parallel-replicas

SET query_plan_optimize_lazy_materialization = 1;
SET query_plan_max_limit_for_lazy_materialization = 10;
SET enable_analyzer = 1;

-- Inspect structured plan nodes so assertions do not depend on text formatting or column order.

DROP TABLE IF EXISTS t_str;
CREATE TABLE t_str (id UInt64, ts DateTime, s String, s_alias String ALIAS s, fat String)
ENGINE = MergeTree ORDER BY id;
INSERT INTO t_str
SELECT number, toDateTime('2026-01-01 00:00:00') + number, toString(number), repeat('x', 1024)
FROM numbers(100);

SET query_plan_merge_expressions = 0;
SELECT 'merge_expressions = 0';

-- Both outputs must contain real data even when they refer to the same source.
SELECT s, s_alias FROM t_str ORDER BY ts DESC LIMIT 10;

-- An alias alone must not make its source eager because of intermediate duplication.
SELECT s_alias FROM t_str ORDER BY ts DESC LIMIT 10;
WITH RECURSIVE plan_nodes AS
(
    SELECT JSONExtractRaw(arrayJoin(JSONExtractArrayRaw(explain)), 'Plan') AS node
    FROM (EXPLAIN PLAN actions = 1, json = 1 SELECT s_alias FROM t_str ORDER BY ts DESC LIMIT 10)
    UNION ALL
    SELECT arrayJoin(JSONExtractArrayRaw(node, 'Plans')) AS node FROM plan_nodes
)
SELECT
    countIf(has(JSONExtract(node, 'Lazily read columns', 'Array(String)'), 's')) AS lazy_s
FROM plan_nodes;

-- Only the duplicated source should be eager; an independent column stays lazy.
SELECT s, s_alias, fat FROM t_str ORDER BY ts DESC LIMIT 10;
WITH RECURSIVE plan_nodes AS
(
    SELECT JSONExtractRaw(arrayJoin(JSONExtractArrayRaw(explain)), 'Plan') AS node
    FROM (EXPLAIN PLAN actions = 1, json = 1 SELECT s, s_alias, fat FROM t_str ORDER BY ts DESC LIMIT 10)
    UNION ALL
    SELECT arrayJoin(JSONExtractArrayRaw(node, 'Plans')) AS node FROM plan_nodes
)
SELECT
    countIf(has(JSONExtract(node, 'Lazily read columns', 'Array(String)'), 's')) AS lazy_s,
    countIf(has(JSONExtract(node, 'Lazily read columns', 'Array(String)'), 'fat')) AS lazy_fat
FROM plan_nodes;

SET query_plan_merge_expressions = 1;
SELECT 'merge_expressions = 1';

-- Both outputs must contain real data even when they refer to the same source.
SELECT s, s_alias FROM t_str ORDER BY ts DESC LIMIT 10;

-- An alias alone must not make its source eager because of intermediate duplication.
SELECT s_alias FROM t_str ORDER BY ts DESC LIMIT 10;
WITH RECURSIVE plan_nodes AS
(
    SELECT JSONExtractRaw(arrayJoin(JSONExtractArrayRaw(explain)), 'Plan') AS node
    FROM (EXPLAIN PLAN actions = 1, json = 1 SELECT s_alias FROM t_str ORDER BY ts DESC LIMIT 10)
    UNION ALL
    SELECT arrayJoin(JSONExtractArrayRaw(node, 'Plans')) AS node FROM plan_nodes
)
SELECT
    countIf(has(JSONExtract(node, 'Lazily read columns', 'Array(String)'), 's')) AS lazy_s
FROM plan_nodes;

-- Only the duplicated source should be eager; an independent column stays lazy.
SELECT s, s_alias, fat FROM t_str ORDER BY ts DESC LIMIT 10;
WITH RECURSIVE plan_nodes AS
(
    SELECT JSONExtractRaw(arrayJoin(JSONExtractArrayRaw(explain)), 'Plan') AS node
    FROM (EXPLAIN PLAN actions = 1, json = 1 SELECT s, s_alias, fat FROM t_str ORDER BY ts DESC LIMIT 10)
    UNION ALL
    SELECT arrayJoin(JSONExtractArrayRaw(node, 'Plans')) AS node FROM plan_nodes
)
SELECT
    countIf(has(JSONExtract(node, 'Lazily read columns', 'Array(String)'), 's')) AS lazy_s,
    countIf(has(JSONExtract(node, 'Lazily read columns', 'Array(String)'), 'fat')) AS lazy_fat
FROM plan_nodes;

DROP TABLE t_str;
