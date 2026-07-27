-- Regression test for a crash (Logical error: 'page_pos < pages.size()') that happened when
-- OPTIMIZE-ing a table whose engine combines rows (Coalescing/Summing/Aggregating/...) and that
-- has a projection carrying the parent _part_offset.
--
-- Such merges reduce rows, so the source-to-merged offset mapping is incomplete and the projection
-- must be rebuilt rather than merged. Before the fix these engines were missing from
-- merge_may_reduce_rows, so the projection was merged and the offset remap read out of bounds.
--
-- The self-join below checks that every rebuilt projection row points back to the base row with the
-- same key, i.e. its stored _parent_part_offset equals the base _part_offset.

DROP TABLE IF EXISTS t_coalescing;
CREATE TABLE t_coalescing
(
    a Int32,
    v Nullable(Int64),
    PROJECTION p (SELECT a, _part_offset ORDER BY a)
)
ENGINE = CoalescingMergeTree
ORDER BY a
SETTINGS deduplicate_merge_projection_mode = 'rebuild';

INSERT INTO t_coalescing SELECT number, number FROM numbers(1000);
INSERT INTO t_coalescing SELECT number, NULL FROM numbers(1000);
OPTIMIZE TABLE t_coalescing FINAL;
SELECT 'coalescing', count() = sum(l._part_offset = r._parent_part_offset)
FROM t_coalescing l JOIN mergeTreeProjection(currentDatabase(), t_coalescing, p) r USING (a)
SETTINGS enable_analyzer = 1;
DROP TABLE t_coalescing;

DROP TABLE IF EXISTS t_summing;
CREATE TABLE t_summing
(
    a Int32,
    v Int64,
    PROJECTION p (SELECT a, _part_offset ORDER BY a)
)
ENGINE = SummingMergeTree
ORDER BY a
SETTINGS deduplicate_merge_projection_mode = 'rebuild';

INSERT INTO t_summing SELECT number, 1 FROM numbers(1000);
INSERT INTO t_summing SELECT number, 10 FROM numbers(1000);
OPTIMIZE TABLE t_summing FINAL;
SELECT 'summing', count() = sum(l._part_offset = r._parent_part_offset)
FROM t_summing l JOIN mergeTreeProjection(currentDatabase(), t_summing, p) r USING (a)
SETTINGS enable_analyzer = 1;
DROP TABLE t_summing;

DROP TABLE IF EXISTS t_aggregating;
CREATE TABLE t_aggregating
(
    a Int32,
    v SimpleAggregateFunction(sum, Int64),
    PROJECTION p (SELECT a, _part_offset ORDER BY a)
)
ENGINE = AggregatingMergeTree
ORDER BY a
SETTINGS deduplicate_merge_projection_mode = 'rebuild';

INSERT INTO t_aggregating SELECT number, 1 FROM numbers(1000);
INSERT INTO t_aggregating SELECT number, 10 FROM numbers(1000);
OPTIMIZE TABLE t_aggregating FINAL;
SELECT 'aggregating', count() = sum(l._part_offset = r._parent_part_offset)
FROM t_aggregating l JOIN mergeTreeProjection(currentDatabase(), t_aggregating, p) r USING (a)
SETTINGS enable_analyzer = 1;
DROP TABLE t_aggregating;
