-- AST-backed predicates do not have an `ActionsDAG` node.
-- Ordinary comparisons make the result independent of predicate ordering.
SET enable_analyzer = 0, optimize_move_to_prewhere = 1;
SET allow_reorder_prewhere_conditions = 1, move_all_conditions_to_prewhere = 1, use_statistics = 0;

DROP TABLE IF EXISTS t_prewhere_ast_null_dag;
CREATE TABLE t_prewhere_ast_null_dag (id UInt64, a UInt64, b UInt64)
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;
INSERT INTO t_prewhere_ast_null_dag VALUES (0, 1, 2), (1, 1, 3), (2, 0, 2), (3, 1, 2);

-- With both plan optimizations enabled, the old analyzer still uses a DAG.
SELECT id FROM t_prewhere_ast_null_dag WHERE a = 1 AND b = 2 ORDER BY id
SETTINGS query_plan_optimize_prewhere = 1, query_plan_enable_optimizations = 1;

-- Disabling either setting selects AST-based PREWHERE optimization.
SELECT id FROM t_prewhere_ast_null_dag WHERE a = 1 AND b = 2 ORDER BY id
SETTINGS query_plan_optimize_prewhere = 0, query_plan_enable_optimizations = 1;

SELECT id FROM t_prewhere_ast_null_dag WHERE a = 1 AND b = 2 ORDER BY id
SETTINGS query_plan_optimize_prewhere = 1, query_plan_enable_optimizations = 0;

DROP TABLE t_prewhere_ast_null_dag;
