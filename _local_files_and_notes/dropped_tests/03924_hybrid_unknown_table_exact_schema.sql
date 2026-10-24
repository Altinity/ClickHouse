SET allow_experimental_hybrid_table = 1,
    enable_analyzer = 1,
    prefer_localhost_replica = 0,
    iceberg_delete_data_on_drop = 1;

DROP TABLE IF EXISTS hybrid_table;
DROP TABLE IF EXISTS merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172;
DROP TABLE IF EXISTS iceberg_table_b4bd039e_d5d3_11f0_8208_e0c26496f172;
DROP TABLE IF EXISTS merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172;
DROP TABLE IF EXISTS hybrid_table_64293f1a_0cba_11f1_876b_de7b9eea3490;
DROP TABLE IF EXISTS merge_tree_table_640a9b6e_0cba_11f1_876b_de7b9eea3490;
DROP TABLE IF EXISTS database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172`;
DROP DATABASE IF EXISTS database_39afd42b_d5d6_11f0_b919_e0c26496f172;

CREATE TABLE merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172
(
    boolean_col Nullable(Bool),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Decimal(10, 2))
)
ENGINE = MergeTree
PARTITION BY string_col
ORDER BY tuple()
SETTINGS index_granularity = 8192;

INSERT INTO merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172 VALUES
    (true, 9044, 2931.782814070929, 'William', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 3733, 7730.6836, 456.78),
    (true, 1654, 3801.2622503916614, 'Oliver', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 3432, 6701.752, 456.78),
    (true, 8751, 7291.267979503492, 'Frank', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 5313, 8428.52, 456.78),
    (true, 1519, 3799.273006373374, 'Louis', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 8785, 1248.2616, 456.78),
    (false, 3611, 4492.090462838536, 'Isaac', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 4552, 1554.795, 456.78);

SELECT 'merge_tree_row_count';
SELECT count() FROM merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172;

CREATE TABLE iceberg_table_b4bd039e_d5d3_11f0_8208_e0c26496f172
(
    boolean_col Nullable(Int32),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Float64)
)
ENGINE = IcebergS3(
    s3_conn,
    filename = concat('hybrid_unknown_table_exact_schema_03924/', currentDatabase(), '/iceberg_table')
);

INSERT INTO iceberg_table_b4bd039e_d5d3_11f0_8208_e0c26496f172 SETTINGS allow_experimental_insert_into_iceberg = 1, write_full_path_in_iceberg_metadata = 1 VALUES
    (1, 9044, 2931.782814070929, 'William', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 3733, 7730.6836, 456.78),
    (1, 1654, 3801.2622503916614, 'Oliver', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 3432, 6701.752, 456.78),
    (1, 8751, 7291.267979503492, 'Frank', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 5313, 8428.52, 456.78),
    (1, 1519, 3799.273006373374, 'Louis', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 8785, 1248.2616, 456.78),
    (0, 3611, 4492.090462838536, 'Isaac', toDateTime64('2024-01-01 06:00:00', 6), toDate('2024-01-01'), 43200000000, toDateTime64('2024-01-01 12:00:00', 6, 'UTC'), 4552, 1554.795, 456.78);

SELECT 'iceberg_row_count';
SELECT count() FROM iceberg_table_b4bd039e_d5d3_11f0_8208_e0c26496f172;

CREATE TABLE hybrid_table
(
    boolean_col Nullable(Bool),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Decimal(10, 2))
)
ENGINE = Hybrid(
    remote('localhost', currentDatabase(), 'merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172'),
    date_col <= '2024-01-01',
    icebergCluster(
        'test_cluster_one_shard_three_replicas_localhost',
        concat('http://localhost:11111/test/hybrid_unknown_table_exact_schema_03924/', currentDatabase(), '/iceberg_table/'),
        'test',
        'testtest'
    ),
    date_col > '2024-01-01'
);

SELECT 'hybrid_row_count';
SELECT count() FROM hybrid_table;

CREATE TABLE merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172
(
    boolean_col Nullable(Bool),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Decimal(10, 2))
)
ENGINE = MergeTree
PARTITION BY string_col
ORDER BY tuple()
SETTINGS index_granularity = 8192;

INSERT INTO merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172
SELECT * FROM merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172;

CREATE DATABASE database_39afd42b_d5d6_11f0_b919_e0c26496f172;

CREATE TABLE database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172`
(
    boolean_col Nullable(Int32),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Float64)
)
ENGINE = IcebergS3(
    s3_conn,
    filename = concat('hybrid_unknown_table_exact_schema_03924/', currentDatabase(), '/iceberg_table_39afe20a_d5d6_11f0_8208_e0c26496f172')
);

INSERT INTO database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172`
SETTINGS allow_experimental_insert_into_iceberg = 1, write_full_path_in_iceberg_metadata = 1
SELECT
    toInt32(boolean_col),
    long_col,
    double_col,
    string_col,
    timestamp_col,
    date_col,
    time_col,
    timestamptz_col,
    integer_col,
    float_col,
    toFloat64(decimal_col)
FROM merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172;

SELECT *
FROM hybrid_table
WHERE string_col IN
(
    SELECT DISTINCT string_col
    FROM hybrid_table
    WHERE long_col > 1500
)
ORDER BY string_col;

SELECT 'issue_1208_join_hybrid_mt_local';
SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
ORDER BY h.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'local';

SELECT 'issue_1208_join_hybrid_mt_allow';
SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
ORDER BY h.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'allow';

SELECT 'issue_1208_join_hybrid_mt_iceberg_local';
SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long,
    i.long_col AS iceberg_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
FULL OUTER JOIN database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172` AS i ON h.string_col = i.string_col
ORDER BY h.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'local'; -- { serverError UNKNOWN_IDENTIFIER }

SELECT 'issue_1208_join_hybrid_mt_iceberg_allow';
SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long,
    i.long_col AS iceberg_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
FULL OUTER JOIN database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172` AS i ON h.string_col = i.string_col
ORDER BY h.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'allow';

-- Exact issue-shape queries (no ORDER BY), deterministic output via FORMAT Null.
SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'local'
FORMAT Null;

SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'allow'
FORMAT Null;

SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long,
    i.long_col AS iceberg_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
FULL OUTER JOIN database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172` AS i ON h.string_col = i.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'local'
FORMAT Null; -- { serverError UNKNOWN_IDENTIFIER }

SELECT
    h.string_col,
    h.long_col AS hybrid_long,
    m.long_col AS mt_long,
    i.long_col AS iceberg_long
FROM hybrid_table AS h
FULL OUTER JOIN merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172 AS m ON h.string_col = m.string_col
FULL OUTER JOIN database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172` AS i ON h.string_col = i.string_col
LIMIT 10
SETTINGS object_storage_cluster_join_mode = 'allow'
FORMAT Null;

CREATE TABLE merge_tree_table_640a9b6e_0cba_11f1_876b_de7b9eea3490
(
    boolean_col Nullable(Bool),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Decimal(10, 2))
)
ENGINE = MergeTree
PARTITION BY string_col
ORDER BY tuple()
SETTINGS index_granularity = 8192;

INSERT INTO merge_tree_table_640a9b6e_0cba_11f1_876b_de7b9eea3490
SELECT * FROM merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172;

CREATE TABLE hybrid_table_64293f1a_0cba_11f1_876b_de7b9eea3490
(
    boolean_col Nullable(Bool),
    long_col Nullable(Int64),
    double_col Nullable(Float64),
    string_col String,
    timestamp_col Nullable(DateTime64(6)),
    date_col Nullable(Date),
    time_col Nullable(Int64),
    timestamptz_col Nullable(DateTime64(6, 'UTC')),
    integer_col Nullable(Int32),
    float_col Nullable(Float32),
    decimal_col Nullable(Decimal(10, 2))
)
ENGINE = Hybrid(
    remote('localhost', currentDatabase(), 'merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172'),
    date_col <= '2024-01-01',
    icebergCluster(
        'test_cluster_one_shard_three_replicas_localhost',
        concat('http://localhost:11111/test/hybrid_unknown_table_exact_schema_03924/', currentDatabase(), '/iceberg_table/'),
        'test',
        'testtest'
    ),
    date_col > '2024-01-01'
);

SELECT *
FROM hybrid_table_64293f1a_0cba_11f1_876b_de7b9eea3490
WHERE string_col IN
(
    SELECT DISTINCT string_col
    FROM merge_tree_table_640a9b6e_0cba_11f1_876b_de7b9eea3490
    WHERE long_col > 1500
)
FORMAT Null;

DROP TABLE hybrid_table;
DROP TABLE merge_tree_table_b9faf88a_d5d3_11f0_b816_e0c26496f172;
DROP TABLE iceberg_table_b4bd039e_d5d3_11f0_8208_e0c26496f172;
DROP TABLE merge_tree_table_3ef2c546_d5d6_11f0_b816_e0c26496f172;
DROP TABLE hybrid_table_64293f1a_0cba_11f1_876b_de7b9eea3490;
DROP TABLE merge_tree_table_640a9b6e_0cba_11f1_876b_de7b9eea3490;
DROP TABLE database_39afd42b_d5d6_11f0_b919_e0c26496f172.`namespace_39afe1b3_d5d6_11f0_9b00_e0c26496f172.table_39afe20a_d5d6_11f0_8208_e0c26496f172`;
DROP DATABASE database_39afd42b_d5d6_11f0_b919_e0c26496f172;
