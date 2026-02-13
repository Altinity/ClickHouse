| Feature                                                                    |                                          Altinity PR                                           | First Altinity Release | upstream PR | First upstream Release |
|----------------------------------------------------------------------------|:----------------------------------------------------------------------------------------------:|:----------------------:| :---: | :---: |
| <   **SECURITY**   >                                                       |                                                                                                |                        ||
| Token-based authentication and authorization                               |                        https://github.com/Altinity/ClickHouse/pull/1078                        |        25.8.12         ||
| ---                                                                        |
| <   **PERFORMANCE**   >                                                    |                                                                                                |                        ||
| Distributed execution: better split tasks by row groups IDs                |                        https://github.com/Altinity/ClickHouse/pull/1237                        |        25.8.12         |https://github.com/ClickHouse/ClickHouse/pull/87508|25.11.1|| Support writing parquet enum as byte array|https://github.com/Altinity/ClickHouse/pull/989|25.6.5|https://github.com/ClickHouse/ClickHouse/pull/81090|25.7.1
| Enable parquet reader v3 by default                                        |                        https://github.com/Altinity/ClickHouse/pull/1232                        |        25.8.12         |https://github.com/ClickHouse/ClickHouse/pull/88827|25.11.1|
| Set max message size on parquet v3 reader                                  |                        https://github.com/Altinity/ClickHouse/pull/1198                        |        25.8.12         |https://github.com/ClickHouse/ClickHouse/pull/91737|25.12.1
| AWS S3 authentication with an explicitly provided IAM role                 |                        https://github.com/Altinity/ClickHouse/pull/986                         |         25.6.5         |https://github.com/ClickHouse/ClickHouse/pull/84011|25.8.1
| Iceberg table pruning in cluster requests                                  |                        https://github.com/Altinity/ClickHouse/pull/770                         |         25.2.2         ||
| ListObjectsV2 cache                                                        |                        https://github.com/Altinity/ClickHouse/pull/743                         |         25.2.2         ||
| Improve performance of hive path parsing                                   |                        https://github.com/Altinity/ClickHouse/pull/734                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/79067|25.5.1
| Lazy load metadata for metadata for DataLake                               |                        https://github.com/Altinity/ClickHouse/pull/742                         |         25.2.2         ||
| Iceberg metadata files cache                                               |                        https://github.com/Altinity/ClickHouse/pull/733                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/77156 |25.5.1
| Support MinMax index for Iceberg                                           |                        https://github.com/Altinity/ClickHouse/pull/733                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/78242|25.4.1
| Parquet file metadata caching: clear cache                                 |                        https://github.com/Altinity/ClickHouse/pull/713                         |         25.2.2         ||
| Parquet: bloom filters support                                             |                                      same as upstream =>                                       |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/62966|24.10.1
| Parquet: page header v2 support on native reader                           |                                      same as upstream =>                                       |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/70807|24.10.1
| Parquet: boolean support on native reader                                  |                                      same as upstream =>                                       |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/71055|24.11.1
| Parquet: merge bloom filter and min/max evaluation                         |                        https://github.com/Altinity/ClickHouse/pull/590                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/71383|25.2.1
| Parquet: Int logical type support on native reader                         |                        https://github.com/Altinity/ClickHouse/pull/589                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/72105|25.1.1
| Parquet file metadata caching                                              |                        https://github.com/Altinity/ClickHouse/pull/586                         |        24.12.2         ||
| Parquet file metadata caching: use cache for parquetmetadata format        |                        https://github.com/Altinity/ClickHouse/pull/636                         |        24.12.2         ||
| Parquet file metadata caching: turn cache on by default                    |https://github.com/Altinity/ClickHouse/pull/669, https://github.com/Altinity/ClickHouse/pull/674|        24.12.2         ||
| <   **SWARMS**   >                                                         |                                                                                                |                        ||
|Profile events for task distribution in ObjectStorageCluster requests|https://github.com/Altinity/ClickHouse/pull/1172|25.8.12||
| SYSTEM STOP SWARM MODE command for graceful shutdown of swarm node|https://github.com/Altinity/ClickHouse/pull/1014|25.6.5||
| JOIN with *Cluster table functions and swarm queries|https://github.com/Altinity/ClickHouse/pull/972|25.6.5||
| Restart cluster tasks on connection lost|https://github.com/Altinity/ClickHouse/pull/780|25.6.5||
| Add iceberg_metadata_file_path to query when send it to swarm nodes|https://github.com/Altinity/ClickHouse/pull/898|25.3.3||
| Setting `lock_object_storage_task_distribution_ms` to improve cache locality with swarm cluster|https://github.com/Altinity/ClickHouse/pull/866|25.3.3||
| Setting object_storage_max_nodes                                           |                        https://github.com/Altinity/ClickHouse/pull/677                         |         25.2.2         ||
| Rendezvous hashing filesystem cache                                        |                        https://github.com/Altinity/ClickHouse/pull/709                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/77326|25.5.1
| Convert functions with object_storage_cluster setting to cluster functions |                        https://github.com/Altinity/ClickHouse/pull/712                         |         25.2.2         ||
| Auxiliary autodiscovery                                                    |                        https://github.com/Altinity/ClickHouse/pull/531                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/71911|24.11.1
| Fix remote call of s3Cluster function                                      |                        https://github.com/Altinity/ClickHouse/pull/583                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/72625|
| Cluster auto discovery                                                     |                        https://github.com/Altinity/ClickHouse/pull/629                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/76001|25.3.1
| Alternative syntax for object storage cluster functions                    |                        https://github.com/Altinity/ClickHouse/pull/592                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/70659|25.3.1
| Limit parsing threads for distibuted case                                  |                        https://github.com/Altinity/ClickHouse/pull/648                         |        24.12.2         ||
| Distributed request to tables with Object Storage Engines                  |                        https://github.com/Altinity/ClickHouse/pull/615                         |        24.12.2         ||
| <   **CATALOGS**   >                                                       |                                                                                                |                        ||
|Add metrics for Iceberg, S3, and Azure|https://github.com/Altinity/ClickHouse/pull/1123|25.8.9||
|Add `icebergLocalCluster` table function to allow cluster reads from shared disk|https://github.com/Altinity/ClickHouse/pull/1120|25.8.9||
|Setting `iceberg_timezone_for_timestamptz` for Iceberg TimestampTZ type|https://github.com/Altinity/ClickHouse/pull/1103|25.8.9||
|Allow to read Iceberg data from any location|https://github.com/Altinity/ClickHouse/pull/1092|25.8.9||
|Read optimization using Iceberg metadata|https://github.com/Altinity/ClickHouse/pull/1019|25.8.9||
|Lazy load metadata for metadata for DataLake|https://github.com/Altinity/ClickHouse/pull/742|25.8.9||
|Allow key-value arguments in s3/s3cluster engine|https://github.com/Altinity/ClickHouse/pull/1028|25.6.5|https://github.com/ClickHouse/ClickHouse/pull/85134|25.8.1|
| Support compressed metadata in Iceberg|https://github.com/Altinity/ClickHouse/pull/1005|25.6.5|https://github.com/ClickHouse/ClickHouse/pull/81451|25.7.1
| Support TimestampTZ in Glue catalog|https://github.com/Altinity/ClickHouse/pull/992|25.6.5|https://github.com/ClickHouse/ClickHouse/issues/83132|25.7.1
| Expose IcebergS3 table metainformation in system.tables|https://github.com/Altinity/ClickHouse/pull/959|25.6.5||
| Support different warehouses behind Iceberg REST catalog|https://github.com/Altinity/ClickHouse/pull/860|25.3.3||
| Support for Iceberg partition pruning bucket transform|https://github.com/Altinity/ClickHouse/pull/786|25.3.3|https://github.com/ClickHouse/ClickHouse/pull/79262|25.5.1
| Unity catalog integration                                                  |                                      same as upstream =>                                       |         25.3.3         |https://github.com/ClickHouse/ClickHouse/pull/76988|25.3.1
| Glue catalog integration                                                   |                                      same as upstream =>                                       |         25.3.3         |https://github.com/ClickHouse/ClickHouse/pull/77257|25.3.1
| General engine definition for Iceberg tables                               |                        https://github.com/Altinity/ClickHouse/pull/675                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/77125|
| RBAC for S3                                                                |                        https://github.com/Altinity/ClickHouse/pull/688                         |         25.2.2         ||
| Iceberg REST Catalog integration                                           |                                      same as upstream =>                                       |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/71542|24.12.1
| <   **TIERED STORAGE**   >                                                 |                                                                                                |                        ||
| <   **TIERED STORAGE**   >||||
|Add ability to split large parquet files on part export|https://github.com/Altinity/ClickHouse/pull/1229|25.8.12||
|Add experimental support to automatically reconcile column-type mismatches across segments in Hybrid table engine |https://github.com/Altinity/ClickHouse/pull/1156|25.8.9||
|Preserve the entire format settings object in export part manifest|https://github.com/Altinity/ClickHouse/pull/1144|25.8.9||
|Export partition support for ReplicatedMergeTree engine|https://github.com/Altinity/ClickHouse/pull/1124|25.8.9||
|Allow any partition strategy to accept part export|https://github.com/Altinity/ClickHouse/pull/1083|25.8.9||
|`Engine=hybrid` implementation|https://github.com/Altinity/ClickHouse/pull/1071|25.8.9||
|Add observability for EXPORT PART|https://github.com/Altinity/ClickHouse/pull/1017|25.6.5||
| Simple MergeTree part export to object storage|https://github.com/Altinity/ClickHouse/pull/1009|25.6.5||
| Support for hive partition style reads and writes|https://github.com/Altinity/ClickHouse/pull/934|25.6.5|https://github.com/ClickHouse/ClickHouse/pull/76802|25.8.1
| Write to Merge tables                                                      |                        https://github.com/Altinity/ClickHouse/pull/683                         |   `25.3.x` (planned)   ||
| Better S3 URL parsing for Hive partitioning                                |                        https://github.com/Altinity/ClickHouse/pull/700                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/78185|25.5.1
| s3Cluster hive partitioning for old analyzer                               |                        https://github.com/Altinity/ClickHouse/pull/703                         |         25.2.2         ||
| Support partition pruning in DeltaLake engine                              |                        https://github.com/Altinity/ClickHouse/pull/733                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/78486|25.4.1
| Iceberg time travel by snapshots                                           |                        https://github.com/Altinity/ClickHouse/pull/733                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/77439|25.4.1
| s3Cluster hive partitioning                                                |                        https://github.com/Altinity/ClickHouse/pull/584                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/73910|


<hr>

*© 2024-2026 Altinity Inc. All rights reserved. Altinity®, Altinity.Cloud®, and Altinity Stable Builds® are registered trademarks of Altinity, Inc. ClickHouse® is a registered trademark of ClickHouse, Inc.*
