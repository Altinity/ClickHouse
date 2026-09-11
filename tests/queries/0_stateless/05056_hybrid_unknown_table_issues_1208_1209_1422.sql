SET allow_experimental_hybrid_table = 1,
    enable_analyzer = 1,
    prefer_localhost_replica = 0,
    -- AST-path regression test for unknown-table issues #1208/#1209/#1422. Pin
    -- serialize_query_plan=0 so the "distributed plan" CI flavor (which forces it on) does not
    -- route these hybrid + IN-subquery queries through the plan path, which has a separate,
    -- unrelated header-reconciliation gap.
    serialize_query_plan = 0;

DROP TABLE IF EXISTS test_hybrid_issue_1208_1209_1422;
DROP TABLE IF EXISTS test_hybrid_issue_1208_1209_1422_left;
DROP TABLE IF EXISTS test_hybrid_issue_1208_1209_1422_right;
DROP TABLE IF EXISTS test_hybrid_issue_1208_1209_1422_mt;

CREATE TABLE test_hybrid_issue_1208_1209_1422_left
(
    string_col String,
    long_col Int64,
    date_col Date
)
ENGINE = MergeTree
ORDER BY string_col;

CREATE TABLE test_hybrid_issue_1208_1209_1422_right
(
    string_col String,
    long_col Int64,
    date_col Date
)
ENGINE = MergeTree
ORDER BY string_col;

CREATE TABLE test_hybrid_issue_1208_1209_1422_mt
(
    string_col String,
    long_col Int64,
    date_col Date
)
ENGINE = MergeTree
ORDER BY string_col;

INSERT INTO test_hybrid_issue_1208_1209_1422_left VALUES
    ('William', 9044, toDate('2024-01-01')),
    ('Oliver', 1654, toDate('2024-01-01')),
    ('Frank', 8751, toDate('2024-01-01'));

INSERT INTO test_hybrid_issue_1208_1209_1422_right VALUES
    ('Louis', 1519, toDate('2024-01-02')),
    ('Isaac', 3611, toDate('2024-01-02'));

INSERT INTO test_hybrid_issue_1208_1209_1422_mt
SELECT * FROM test_hybrid_issue_1208_1209_1422_left
UNION ALL
SELECT * FROM test_hybrid_issue_1208_1209_1422_right;

CREATE TABLE test_hybrid_issue_1208_1209_1422
(
    string_col String,
    long_col Int64,
    date_col Date
)
ENGINE = Hybrid(
    remote('127.0.0.1:9000', currentDatabase(), 'test_hybrid_issue_1208_1209_1422_left'), date_col <= '2024-01-01',
    remote('127.0.0.1:9000', currentDatabase(), 'test_hybrid_issue_1208_1209_1422_right'), date_col > '2024-01-01'
);

SELECT 'issue_1208_self_in_subquery';
SELECT count()
FROM
(
    SELECT string_col
    FROM test_hybrid_issue_1208_1209_1422
    WHERE string_col IN
    (
        SELECT DISTINCT string_col
        FROM test_hybrid_issue_1208_1209_1422
        WHERE long_col > 1500
    )
);

SELECT 'issue_1209_join_mode_local';
SELECT uniqExact(coalesce(h_string_col, m_string_col))
FROM
(
    SELECT h.string_col AS h_string_col, m.string_col AS m_string_col, h.long_col AS hybrid_long, m.long_col AS mt_long
    FROM test_hybrid_issue_1208_1209_1422 AS h
    FULL OUTER JOIN test_hybrid_issue_1208_1209_1422_mt AS m ON h.string_col = m.string_col
    SETTINGS object_storage_cluster_join_mode = 'local'
);

SELECT 'issue_1209_join_mode_allow';
SELECT uniqExact(coalesce(h_string_col, m_string_col))
FROM
(
    SELECT h.string_col AS h_string_col, m.string_col AS m_string_col, h.long_col AS hybrid_long, m.long_col AS mt_long
    FROM test_hybrid_issue_1208_1209_1422 AS h
    FULL OUTER JOIN test_hybrid_issue_1208_1209_1422_mt AS m ON h.string_col = m.string_col
    SETTINGS object_storage_cluster_join_mode = 'allow'
);

SELECT 'issue_1422_hybrid_in_merge_tree_subquery';
SELECT count()
FROM
(
    SELECT string_col
    FROM test_hybrid_issue_1208_1209_1422
    WHERE string_col IN
    (
        SELECT DISTINCT string_col
        FROM test_hybrid_issue_1208_1209_1422_mt
        WHERE long_col > 1500
    )
);

DROP TABLE test_hybrid_issue_1208_1209_1422;
DROP TABLE test_hybrid_issue_1208_1209_1422_left;
DROP TABLE test_hybrid_issue_1208_1209_1422_right;
DROP TABLE test_hybrid_issue_1208_1209_1422_mt;
