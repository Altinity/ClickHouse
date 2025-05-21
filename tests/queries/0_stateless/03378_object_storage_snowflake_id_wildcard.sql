DROP TABLE IF EXISTS t_03378_parquet, t_03378_csv;

CREATE TABLE t_03378_parquet (year UInt16, country String, counter UInt8)
ENGINE = S3(s3_conn, filename = 't_03378_parquet/{_partition_id}/{_snowflake_id}.parquet', format = Parquet)
PARTITION BY (year, country);

INSERT INTO t_03378_parquet VALUES
    (2022, 'USA', 1),
    (2022, 'Canada', 2),
    (2023, 'USA', 3),
    (2023, 'Mexico', 4),
    (2024, 'France', 5),
    (2024, 'Germany', 6),
    (2024, 'Germany', 7),
    (1999, 'Brazil', 8),
    (2100, 'Japan', 9),
    (2024, 'CN', 10),
    (2025, '', 11);

select distinct on (counter) replaceRegexpAll(_path, '/[0-9]+\\.parquet', '/<snowflakeid>.parquet') AS _path, counter from t_03378_parquet order by counter SETTINGS object_storage_treat_key_related_wildcards_as_star=1;

CREATE TABLE t_03378_csv (year UInt16, country String, counter UInt8)
ENGINE = S3(s3_conn, filename = 't_03378_csv/{_partition_id}/{_snowflake_id}.csv', format = CSV)
PARTITION BY (year, country);

INSERT INTO t_03378_csv VALUES
    (2022, 'USA', 1),
    (2022, 'Canada', 2),
    (2023, 'USA', 3),
    (2023, 'Mexico', 4),
    (2024, 'France', 5),
    (2024, 'Germany', 6),
    (2024, 'Germany', 7),
    (1999, 'Brazil', 8),
    (2100, 'Japan', 9),
    (2024, 'CN', 10),
    (2025, '', 11);

select distinct on (counter) replaceRegexpAll(_path, '/[0-9]+\\.csv', '/<snowflakeid>.csv') AS _path, counter from t_03378_csv order by counter SETTINGS object_storage_treat_key_related_wildcards_as_star=1;

-- s3 table function
INSERT INTO FUNCTION s3(s3_conn, filename='t_03378_function/{_partition_id}/{_snowflake_id}.parquet', format=Parquet) PARTITION BY (year, country) SELECT country, year, counter FROM t_03378_parquet SETTINGS object_storage_treat_key_related_wildcards_as_star=1;;
select distinct on (counter) replaceRegexpAll(_path, '/[0-9]+\\.parquet', '/<snowflakeid>.parquet') AS _path, counter from s3(s3_conn, filename='t_03378_function/**.parquet') order by counter SETTINGS object_storage_treat_key_related_wildcards_as_star=1;
select distinct on (counter) replaceRegexpAll(_path, '/[0-9]+\\.parquet', '/<snowflakeid>.parquet') AS _path, counter from s3(s3_conn, filename='t_03378_function/{_partition_id}/{_snowflake_id}.parquet') order by counter SETTINGS object_storage_treat_key_related_wildcards_as_star=1;
