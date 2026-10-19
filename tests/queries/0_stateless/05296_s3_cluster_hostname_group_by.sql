-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
-- https://github.com/Altinity/ClickHouse/issues/2139

INSERT INTO FUNCTION s3('http://localhost:11111/test/05296_s3_cluster_hostname_group_by.tsv', 'TSV', 'x UInt32')
SELECT number FROM numbers(10)
SETTINGS s3_truncate_on_insert = 1;

SELECT host != '', c FROM
(
    SELECT hostName() AS host, count() AS c
    FROM s3Cluster('test_cluster_two_shards_localhost', 'http://localhost:11111/test/05296_s3_cluster_hostname_group_by.tsv', 'TSV', 'x UInt32')
    GROUP BY host
)
SETTINGS enable_analyzer = 0;

SELECT host != '', c FROM
(
    SELECT hostName() AS host, count() AS c
    FROM s3('http://localhost:11111/test/05296_s3_cluster_hostname_group_by.tsv', 'TSV', 'x UInt32')
    GROUP BY host
    SETTINGS object_storage_cluster = 'test_cluster_two_shards_localhost'
)
SETTINGS enable_analyzer = 0;

SELECT host != '', c FROM
(
    SELECT hostName() AS host, count() AS c
    FROM s3Cluster('test_cluster_two_shards_localhost', 'http://localhost:11111/test/05296_s3_cluster_hostname_group_by.tsv', 'TSV', 'x UInt32')
    GROUP BY host
)
SETTINGS enable_analyzer = 1;
