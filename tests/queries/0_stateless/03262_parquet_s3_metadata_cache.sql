-- Tags: no-parallel, no-fasttest

DROP TABLE IF EXISTS t_parquet_03262;

CREATE TABLE t_parquet_03262 (a UInt64)
ENGINE = S3(s3_conn, filename = 'test_03262_{_partition_id}', format = Parquet)
PARTITION BY a;

INSERT INTO t_parquet_03262 SELECT number FROM numbers(10) SETTINGS s3_truncate_on_insert=1;

SELECT COUNT(*)
FROM s3(s3_conn, filename = 'test_03262_*', format = Parquet)
SETTINGS parquet_use_metadata_cache=1;

SELECT COUNT(*)
FROM s3(s3_conn, filename = 'test_03262_*', format = Parquet)
SETTINGS parquet_use_metadata_cache=1,custom_x='test03262';

SYSTEM FLUSH LOGS;

SELECT ProfileEvents['ParquetMetaDataCacheHits']
FROM system.query_log
where query like '%test03262%'
AND type = 'QueryFinish'
ORDER BY event_time desc
LIMIT 1;

DROP TABLE t_parquet_03262;
