| Feature | Altinity PR | First Altinity Release | upstream PR | First upstream Release |
| ------- | :---: | :---: | :---: | :---: |
| <   **PERFORMANCE**   >||||
| Iceberg table pruning in cluster requests|https://github.com/Altinity/ClickHouse/pull/770|25.2.2||
| ListObjectsV2 cache|https://github.com/Altinity/ClickHouse/pull/743|25.2.2||
| Improve performance of hive path parsing|https://github.com/Altinity/ClickHouse/pull/734|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/79067|25.5.1
| Lazy load metadata for metadata for DataLake|https://github.com/Altinity/ClickHouse/pull/742|25.2.2||
| Iceberg metadata files cache|https://github.com/Altinity/ClickHouse/pull/733|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/77156 |25.5.1
| Support MinMax index for Iceberg|https://github.com/Altinity/ClickHouse/pull/733|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/78242|25.4.1
| Parquet file metadata caching: clear cache|https://github.com/Altinity/ClickHouse/pull/713|25.2.2||
| Parquet: bloom filters support|same as upstream =>|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/62966|24.10.1
| Parquet: page header v2 support on native reader|same as upstream =>|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/70807|24.10.1
| Parquet: boolean support on native reader|same as upstream =>|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/71055|24.11.1
| Parquet: merge bloom filter and min/max evaluation|https://github.com/Altinity/ClickHouse/pull/590|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/71383|25.2.1
| Parquet: Int logical type support on native reader|https://github.com/Altinity/ClickHouse/pull/589|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/72105|25.1.1
| Parquet file metadata caching|https://github.com/Altinity/ClickHouse/pull/586|24.12.2||
| Parquet file metadata caching: use cache for parquetmetadata format|https://github.com/Altinity/ClickHouse/pull/636|24.12.2||
| Parquet file metadata caching: turn cache on by default|https://github.com/Altinity/ClickHouse/pull/669, https://github.com/Altinity/ClickHouse/pull/674|24.12.2||
| <   **SWARMS**   >||||
| Restart cluster tasks on connection lost|https://github.com/Altinity/ClickHouse/pull/780|`25.3.3` (planned)||
| Setting object_storage_max_nodes|https://github.com/Altinity/ClickHouse/pull/677|25.2.2||
| Rendezvous hashing filesystem cache|https://github.com/Altinity/ClickHouse/pull/709|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/77326|25.5.1
| Convert functions with object_storage_cluster setting to cluster functions|https://github.com/Altinity/ClickHouse/pull/712|25.2.2||
| Auxiliary autodiscovery|https://github.com/Altinity/ClickHouse/pull/531|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/71911|24.11.1
| Fix remote call of s3Cluster function|https://github.com/Altinity/ClickHouse/pull/583|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/72625|
| Cluster auto discovery|https://github.com/Altinity/ClickHouse/pull/629|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/76001|25.3.1
| Alternative syntax for object storage cluster functions|https://github.com/Altinity/ClickHouse/pull/592|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/70659|25.3.1
| Limit parsing threads for distibuted case|https://github.com/Altinity/ClickHouse/pull/648|24.12.2||
| Distributed request to tables with Object Storage Engines|https://github.com/Altinity/ClickHouse/pull/615|24.12.2||
| <   **CATALOGS**   >||||
| Unity catalog integration||`25.3.3` (planned)|https://github.com/ClickHouse/ClickHouse/pull/76988|25.3.1
| Glue catalog integration||`25.3.3` (planned)|https://github.com/ClickHouse/ClickHouse/pull/77257|25.3.1
| General engine definition for Iceberg tables|https://github.com/Altinity/ClickHouse/pull/675|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/77125|
| RBAC for S3|https://github.com/Altinity/ClickHouse/pull/688|25.2.2||
| Iceberg REST Catalog integration|same as upstream =>|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/71542|24.12.1
| <   **TIERED STORAGE**   >||||
| Wildcard support for object storage|https://github.com/Altinity/ClickHouse/pull/789|`25.3.3` (planned)||
| Write to Merge tables|https://github.com/Altinity/ClickHouse/pull/683|`25.3.3` (planned)||
| Better S3 URL parsing for Hive partitioning|https://github.com/Altinity/ClickHouse/pull/700|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/78185|25.5.1
| s3Cluster hive partitioning for old analyzer|https://github.com/Altinity/ClickHouse/pull/703|25.2.2||
| Support partition pruning in DeltaLake engine|https://github.com/Altinity/ClickHouse/pull/733|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/78486|25.4.1
| Iceberg time travel by snapshots|https://github.com/Altinity/ClickHouse/pull/733|25.2.2|https://github.com/ClickHouse/ClickHouse/pull/77439|25.4.1
| s3Cluster hive partitioning|https://github.com/Altinity/ClickHouse/pull/584|24.12.2|https://github.com/ClickHouse/ClickHouse/pull/73910|
