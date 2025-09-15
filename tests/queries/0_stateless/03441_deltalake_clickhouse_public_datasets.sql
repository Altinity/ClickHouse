-- Tags: no-fasttest, no-msan
-- Tag no-fasttest: Depends on AWS
-- Tag no-msan: delta-kernel is not built with msan

SELECT count()
FROM deltaLake('https://s3.dualstack.eu-central-1.amazonaws.com/clickhouse-public-datasets/delta_lake/hits/', NOSIGN, SETTINGS allow_experimental_delta_kernel_rs = 1);
