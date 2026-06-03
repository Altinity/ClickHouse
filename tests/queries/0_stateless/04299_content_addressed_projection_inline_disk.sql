-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- Projections on a content_addressed disk: the projection's files are stored as nested keys
-- (<proj>.proj/<file>) in the parent part's manifest. Verify INSERT writes a projection, a
-- projection-optimized SELECT returns correct results, and a merge (OPTIMIZE FINAL) rebuilds it.

DROP TABLE IF EXISTS t_proj_cas;

CREATE TABLE t_proj_cas (a UInt64, b UInt64, PROJECTION p_by_b (SELECT a, b ORDER BY b))
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04299_content_addressed_projection',
    path = '04299_content_addressed_projection_pool/');

INSERT INTO t_proj_cas SELECT number, number % 10 FROM numbers(1000);
INSERT INTO t_proj_cas SELECT number, number % 10 FROM numbers(1000, 1000);

SELECT 'count', count() FROM t_proj_cas;
SELECT 'sum_b', sum(b) FROM t_proj_cas;
SELECT 'by_b', b, count() FROM t_proj_cas GROUP BY b ORDER BY b;

OPTIMIZE TABLE t_proj_cas FINAL;
SELECT 'after_merge_count', count() FROM t_proj_cas;
SELECT 'after_merge_by_b', b, count() FROM t_proj_cas GROUP BY b ORDER BY b;

SELECT 'has_projection', countDistinct(name) FROM system.projection_parts
WHERE database = currentDatabase() AND table = 't_proj_cas' AND active;

-- Prove the projection is actually selected by the optimizer (not a silent base-table fallback).
SET optimize_use_projections = 1, force_optimize_projection = 1;
SELECT 'uses_projection', countIf(explain LIKE '%p_by_b%') > 0
FROM (EXPLAIN actions = 1 SELECT b, count() FROM t_proj_cas GROUP BY b);

DROP TABLE t_proj_cas;
SELECT 'dropped_ok';
