-- Tags: no-fasttest

-- Antalya-specific: this branch enables the Parquet v3 reader by default, so a row policy that
-- reaches the format reader alongside a PREWHERE gives Parquet::Reader::applyPrewhere two
-- filtering steps. It used to abort with
--   Logical error: 'filter.size() == row_subgroup.filter.rows_pass'
-- because a column first needed by the second step was formed from its decoded subchunk, which
-- still had rows_total rows, while the first step had already filtered everything else to
-- rows_pass.
--
-- The policy condition here is a BARE COLUMN on purpose: the filter DAG's output is then named
-- `flag`, a physical column, so it passes supportedPrewhereColumns() and is routed into
-- row_level_filter rather than into WHERE. The PREWHERE/WHERE below is on a DIFFERENT column, so
-- that column is not an input to the row-policy step and is only formed by the second step.

SET input_format_parquet_use_native_reader_v3 = 1;
-- Pin the analyzer: the routing assertions below check the supportedPrewhereColumns() guard,
-- which lives in PlannerJoinTree and so only applies to the new analyzer. The old analyzer
-- (InterpreterSelectQuery) routes every row policy to row_level_filter whenever the storage
-- supports prewhere, so the expression-policy assertion would differ under enable_analyzer = 0.
SET enable_analyzer = 1;

DROP ROW POLICY IF EXISTS p_flag ON t_parquet_rls;
DROP ROW POLICY IF EXISTS p_expr ON t_parquet_rls;
DROP TABLE IF EXISTS t_parquet_rls;

-- Must be created with the v3 reader enabled: StorageFile caches supports_prewhere in its
-- constructor, and that is what decides whether the policy may go to prewhere at all.
CREATE TABLE t_parquet_rls (id UInt32, value String, flag UInt8) ENGINE = File(Parquet);

INSERT INTO t_parquet_rls VALUES
    (1, 'a', 1), (2, 'b', 1), (3, 'c', 0), (4, 'a', 1), (5, 'b', 0),
    (6, 'c', 1), (7, 'a', 0), (8, 'b', 1), (9, 'c', 0), (10, 'a', 1);

CREATE ROW POLICY p_flag ON t_parquet_rls FOR SELECT USING flag TO ALL;

SELECT '-- policy is active (6 of 10 rows have flag = 1)';
SELECT count() FROM t_parquet_rls;

SELECT '-- row policy + WHERE moved to prewhere';
-- Two steps: row-level filter on `flag`, then prewhere on `value`.
SELECT * FROM t_parquet_rls WHERE value = 'a' ORDER BY id;

SELECT '-- row policy + explicit PREWHERE';
SELECT * FROM t_parquet_rls PREWHERE value = 'a' ORDER BY id;

-- Pin the routing the two-step case depends on: without this the queries above keep returning
-- the right rows even if the policy stopped reaching the reader, and the coverage would be lost
-- silently. Note LIKE is case-sensitive, so '%Filter column:%' does not match the reader's
-- 'Row level filter column:'.
SELECT '-- routing: bare-column policy reaches the reader as a row-level filter';
SELECT
    countIf(explain LIKE '%Row level filter column:%') = 1 AS row_policy_in_reader,
    countIf(explain LIKE '%Filter column:%')           = 0 AS row_policy_not_external,
    countIf(explain LIKE '%Prewhere filter column:%')  = 1 AS prewhere_in_reader
FROM (EXPLAIN actions = 1 SELECT * FROM t_parquet_rls PREWHERE value = 'a');

SELECT '-- row policy alone (single step)';
SELECT * FROM t_parquet_rls ORDER BY id;

DROP ROW POLICY p_flag ON t_parquet_rls;

-- An expression-valued policy is diverted to WHERE by the supportedPrewhereColumns() check, so
-- only the prewhere step reaches the reader. This is the common shape and the one that regressed
-- first; keep it covered so the routing guard does not silently stop working.
CREATE ROW POLICY p_expr ON t_parquet_rls FOR SELECT USING id <= 4 TO ALL;

SELECT '-- expression policy + WHERE (policy routed to WHERE)';
SELECT * FROM t_parquet_rls WHERE value = 'a' ORDER BY id;

SELECT '-- routing: expression policy is diverted to WHERE, only prewhere reaches the reader';
SELECT
    countIf(explain LIKE '%Row level filter column:%') = 0 AS row_policy_not_in_reader,
    countIf(explain LIKE '%Filter column:%')           = 1 AS row_policy_is_external_filter,
    countIf(explain LIKE '%Prewhere filter column:%')  = 1 AS prewhere_in_reader
FROM (EXPLAIN actions = 1 SELECT * FROM t_parquet_rls PREWHERE value = 'a');

DROP ROW POLICY p_expr ON t_parquet_rls;
DROP TABLE t_parquet_rls;
