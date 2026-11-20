| Feature                                                                    |                                          Altinity PR                                           | First Altinity Release | upstream PR | First upstream Release |
|----------------------------------------------------------------------------|:----------------------------------------------------------------------------------------------:|:----------------------:| :---: | :---: |
|:arrow_forward:  SECURITY|                                                                                                |                        ||
|Map OAuth groups to settings profiles with `profiles_mapping`|https://github.com/Altinity/ClickHouse/pull/2664|||
|Forward authentication tokens to data lake catalogs|https://github.com/Altinity/ClickHouse/pull/2329|26.6.8||
|Google identity provider for token-based authentication|https://github.com/Altinity/ClickHouse/pull/2499|26.6.8||
|External JWT acquisition with `clickhouse-client --jwt-command`|https://github.com/Altinity/ClickHouse/pull/1809|26.3.10||
|Microsoft Entra ID authentication through OIDC and local JWKS validation|https://github.com/Altinity/ClickHouse/pull/1784|26.3.10||
|Add OAuth2 login flow to `clickhouse-client`|https://github.com/Altinity/ClickHouse/pull/1606|26.1.11||
|Add support for EC JWT validation|https://github.com/Altinity/ClickHouse/pull/1596|26.1.6||
| Token-based authentication and authorization                               |https://github.com/Altinity/ClickHouse/pull/1078|        25.8.12         |https://github.com/ClickHouse/ClickHouse/pull/89978, https://github.com/ClickHouse/ClickHouse/pull/89995|
| ---                                                                        |
|:arrow_forward:  PERFORMANCE|                                                                                                |                        ||
|Reuse AES-GCM cipher contexts across rows; up to 27x faster multithreaded encryption and decryption|https://github.com/Altinity/ClickHouse/pull/2898, https://github.com/Altinity/ClickHouse/pull/2900||https://github.com/ClickHouse/ClickHouse/pull/131001|
|Reuse Puffin object metadata across Iceberg deletion-vector reads to reduce HEAD requests|https://github.com/Altinity/ClickHouse/pull/2419|26.6.8|https://github.com/ClickHouse/ClickHouse/pull/121786|
|Parquet v3 read-ahead and decoding with separate memory and thread budgets|https://github.com/Altinity/ClickHouse/pull/2235|26.6.2||
|Reduce partition-export lock contention with multiversion local manifests|https://github.com/Altinity/ClickHouse/pull/1912|26.3.13||
|Avoid holding export-manifest locks during slow commits|https://github.com/Altinity/ClickHouse/pull/1813|26.3.10||
|Skip remote `DESCRIBE TABLE` when the data lake schema is already known|https://github.com/Altinity/ClickHouse/pull/1845|26.3.10||
|Reduce lock contention in the Iceberg cluster iterator|https://github.com/Altinity/ClickHouse/pull/1436, https://github.com/Altinity/ClickHouse/pull/1497|25.8.16||
|Read optimization using Iceberg metadata|https://github.com/Altinity/ClickHouse/pull/1019|25.8.9|https://github.com/ClickHouse/ClickHouse/pull/90001|
| Iceberg table pruning in cluster requests                         |https://github.com/Altinity/ClickHouse/pull/770           |25.2.2         ||
| ListObjectsV2 cache                                                        |https://github.com/Altinity/ClickHouse/pull/743                         |         25.2.2         ||
| Lazy loading of data lake metadata                               |https://github.com/Altinity/ClickHouse/pull/742                         |         25.2.2         ||
| Parquet file metadata caching: clear cache                                 |https://github.com/Altinity/ClickHouse/pull/713                         |         25.2.2         ||
| Parquet file metadata caching                                              |https://github.com/Altinity/ClickHouse/pull/586                         |        24.12.2         ||
| Use the Parquet metadata cache for the ParquetMetadata format        |https://github.com/Altinity/ClickHouse/pull/636                         |        24.12.2         ||
| ---                                                                        |
|:arrow_forward:  SWARMS|                                                                                                |                        ||
|Initiator-local JOIN and IN with `object_storage_cluster_join_mode=allow`, including the old analyzer|https://github.com/Altinity/ClickHouse/pull/2228|26.6.8||
|Binary Antalya protocol for Iceberg metadata and task-distribution settings|https://github.com/Altinity/ClickHouse/pull/2378, https://github.com/Altinity/ClickHouse/pull/2482, https://github.com/Altinity/ClickHouse/pull/2489|26.6.8||
|Allow empty object-storage clusters|https://github.com/Altinity/ClickHouse/pull/2221|26.6.8||
|Reload cluster discovery settings from `remote_servers` without restart|https://github.com/Altinity/ClickHouse/pull/2197|26.6.4|https://github.com/ClickHouse/ClickHouse/pull/113669|
|Use `object_storage_remote_initiator` without `object_storage_cluster` on the initial node|https://github.com/Altinity/ClickHouse/pull/1608|26.1.11||
|Cluster Joins part 2 - global mode|https://github.com/Altinity/ClickHouse/pull/1527|26.1.11||
|Remote initiator credential and query-setting propagation|https://github.com/Altinity/ClickHouse/pull/1577|26.1.6||
|Profile events for task distribution in ObjectStorageCluster requests|https://github.com/Altinity/ClickHouse/pull/1172|25.8.12||
| SYSTEM STOP SWARM MODE command for graceful shutdown of a swarm node|https://github.com/Altinity/ClickHouse/pull/1014|25.6.5||
| JOIN with *Cluster table functions and swarm queries|https://github.com/Altinity/ClickHouse/pull/972|25.6.5||
| Restart cluster tasks on connection loss|https://github.com/Altinity/ClickHouse/pull/780|25.6.5||
| Send `iceberg_metadata_file_path` to swarm nodes|https://github.com/Altinity/ClickHouse/pull/898|25.3.3||
| Improve swarm cache locality with `lock_object_storage_task_distribution_ms`|https://github.com/Altinity/ClickHouse/pull/866|25.3.3||
|Remote initiator selection with `object_storage_remote_initiator`|https://github.com/Altinity/ClickHouse/pull/756|25.3.3||
| Limit participating nodes with `object_storage_max_nodes`                                           |                        https://github.com/Altinity/ClickHouse/pull/677                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/74525|
| Convert functions with `object_storage_cluster` to cluster functions |                        https://github.com/Altinity/ClickHouse/pull/712                         |         25.2.2         ||
| Distributed requests to tables with Object Storage Engines                  |                        https://github.com/Altinity/ClickHouse/pull/615                         |        24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/76516|
| Alternative syntax for object-storage cluster functions           |https://github.com/Altinity/ClickHouse/pull/592           |24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/74615|
| ---                                                                        |
|:arrow_forward:  CATALOGS|                                                                                                |                        ||
|Iceberg DROP PARTITION for catalog tables, data purge, mixed manifests, equality deletes and partition evolution|https://github.com/Altinity/ClickHouse/pull/2613|||
|Apache DataSketches-compatible `uniqApacheHLL` aggregate function and sketch import/export helpers|https://github.com/Altinity/ClickHouse/pull/2398, https://github.com/Altinity/ClickHouse/pull/2544|26.6.8|https://github.com/ClickHouse/ClickHouse/pull/124667|
|Read Iceberg v3 multi-column transforms with `source-ids`|https://github.com/Altinity/ClickHouse/pull/2318|26.6.8||
|Read and write the Iceberg v3 `unknown` data type|https://github.com/Altinity/ClickHouse/pull/2363|26.6.8||
|Native CREATE TABLE, CREATE TABLE AS and DROP TABLE in DataLakeCatalog, with optional data purge|https://github.com/Altinity/ClickHouse/pull/2305|26.6.8|https://github.com/ClickHouse/ClickHouse/pull/98670|
|FIRST and AFTER clauses for Iceberg ADD COLUMN and MODIFY COLUMN|https://github.com/Altinity/ClickHouse/pull/2289|26.6.8||
|Support for Databricks deletion vectors|https://github.com/Altinity/ClickHouse/pull/2271|26.6.4||
|Data lake catalog authentication-token profile events|https://github.com/Altinity/ClickHouse/pull/2222|26.6.4||
|Iceberg deletion vectors support|https://github.com/Altinity/ClickHouse/pull/2183|26.6.2|https://github.com/ClickHouse/ClickHouse/pull/113561|
|Cache REST catalog vended credentials with `vended_credentials_cache_ttl`|https://github.com/Altinity/ClickHouse/pull/1923|26.3.13|https://github.com/ClickHouse/ClickHouse/pull/107960|
|Profile events for Iceberg REST catalogs|https://github.com/Altinity/ClickHouse/pull/1868|26.3.13|https://github.com/ClickHouse/ClickHouse/pull/107194|
|Read and write the Iceberg `time` type|https://github.com/Altinity/ClickHouse/pull/1761|26.3.10|https://github.com/ClickHouse/ClickHouse/pull/105540|
|DataLakeCatalog namespace filter|https://github.com/Altinity/ClickHouse/pull/1337|25.8.16|https://github.com/ClickHouse/ClickHouse/pull/95388|
|Set the Iceberg partition time zone with `iceberg_partition_timezone`|https://github.com/Altinity/ClickHouse/pull/1349|25.8.16|https://github.com/ClickHouse/ClickHouse/pull/95659|
|Metrics for Iceberg, S3 and Azure|https://github.com/Altinity/ClickHouse/pull/1123|25.8.9|https://github.com/ClickHouse/ClickHouse/pull/93332|
|Set the Iceberg TimestampTZ time zone with `iceberg_timezone_for_timestamptz`|https://github.com/Altinity/ClickHouse/pull/1103|25.8.9|https://github.com/ClickHouse/ClickHouse/pull/94025|
|Read Iceberg data from external locations and different object stores|https://github.com/Altinity/ClickHouse/pull/1092|25.8.9|https://github.com/ClickHouse/ClickHouse/pull/90740|
| Expose IcebergS3 partition and sorting keys in system.tables|https://github.com/Altinity/ClickHouse/pull/959|25.6.5|https://github.com/ClickHouse/ClickHouse/pull/85571|
| Support different warehouses behind the Iceberg REST catalog|https://github.com/Altinity/ClickHouse/pull/860|25.3.3||
|Iceberg database alias for DataLakeCatalog with a REST catalog|https://github.com/Altinity/ClickHouse/pull/822|25.3.3||
| General engine definition for Iceberg tables                               |                        https://github.com/Altinity/ClickHouse/pull/675                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/77125|
| Role-based access for S3                                                                |                        https://github.com/Altinity/ClickHouse/pull/688                         |         25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/77747|
| ---                                                                        |
|:arrow_forward:  TIERED STORAGE|                                                                                                |                        ||
|Iceberg data-file compaction with bin-packing RewriteFiles|https://github.com/Altinity/ClickHouse/pull/2420|26.6.8||
|Export partitions from non-replicated MergeTree tables|https://github.com/Altinity/ClickHouse/pull/2290|26.6.8||
|Safe unsigned-to-signed type promotion for part and partition exports|https://github.com/Altinity/ClickHouse/pull/2559|26.6.8||
|Export across compatible, non-matching partition expressions|https://github.com/Altinity/ClickHouse/pull/2074|26.6.2||
|Match export columns by name or position and optionally ignore extra source columns|https://github.com/Altinity/ClickHouse/pull/2111, https://github.com/Altinity/ClickHouse/pull/2229, https://github.com/Altinity/ClickHouse/pull/2220|26.6.2||
|Expose Iceberg metadata and data-file paths in system.replicated_partition_exports|https://github.com/Altinity/ClickHouse/pull/1832|26.6.2||
|Automatic type conversion and optional lossy casts for part and partition exports|https://github.com/Altinity/ClickHouse/pull/1779|26.3.13||
|EXPORT PARTITION ALL|https://github.com/Altinity/ClickHouse/pull/1741|26.3.10||
|Prune unreachable Hybrid table segments using query predicates|https://github.com/Altinity/ClickHouse/pull/1788|26.3.10||
|Inspect Hybrid table watermarks in system.hybrid_watermarks|https://github.com/Altinity/ClickHouse/pull/1789|26.3.10||
|Move Hybrid table watermarks with ALTER TABLE MODIFY SETTING|https://github.com/Altinity/ClickHouse/pull/1659|26.1.11||
|Background partition export to Apache Iceberg|https://github.com/Altinity/ClickHouse/pull/1618|26.1.11||
|Improve partition-export scheduling and system.replicated_partition_exports performance|https://github.com/Altinity/ClickHouse/pull/1402|26.1.6||
|TRUNCATE TABLE for the Iceberg database engine with REST catalogs|https://github.com/Altinity/ClickHouse/pull/1529|26.1.6|https://github.com/ClickHouse/ClickHouse/pull/102065|
|Define the filename pattern for part exports|https://github.com/Altinity/ClickHouse/pull/1490, https://github.com/Altinity/ClickHouse/pull/1512|25.8.16||
|Add query IDs to system.part_log, system_exports and system.replicated_partition_exports|https://github.com/Altinity/ClickHouse/pull/1330|25.8.14||
|Export MergeTree materialized and ALIAS columns|https://github.com/Altinity/ClickHouse/pull/1324|25.8.14||
|Use a table function as the destination for part export|https://github.com/Altinity/ClickHouse/pull/1320|25.8.14||
|Control export behavior for pending mutations and patch parts|https://github.com/Altinity/ClickHouse/pull/1294|25.8.14||
|Support ALIAS columns in Hybrid table segments|https://github.com/Altinity/ClickHouse/pull/1272|25.8.14||
|Split large Parquet files during part export|https://github.com/Altinity/ClickHouse/pull/1229|25.8.12||
|Automatically reconcile column-type mismatches across Hybrid table segments |https://github.com/Altinity/ClickHouse/pull/1156|25.8.9||
|Preserve format settings in the export-part manifest|https://github.com/Altinity/ClickHouse/pull/1144|25.8.9||
|Partition export for ReplicatedMergeTree|https://github.com/Altinity/ClickHouse/pull/1124|25.8.9||
|Allow any partition strategy for part export|https://github.com/Altinity/ClickHouse/pull/1083|25.8.9||
|Hybrid table engine|https://github.com/Altinity/ClickHouse/pull/1071|25.8.9||
|Observability for EXPORT PART|https://github.com/Altinity/ClickHouse/pull/1017|25.6.5||
| MergeTree part export to object storage|https://github.com/Altinity/ClickHouse/pull/1009|25.6.5|https://github.com/ClickHouse/ClickHouse/pull/91228|
| ---                                                                        |
|:arrow_forward:  CAS|                                                                                                |                        ||
|Reduce CAS write round trips: INSERT uses 14 object-store requests instead of 25|https://github.com/Altinity/ClickHouse/pull/2831|||
|Rename CAS parts with one ref-log owner Move instead of republishing manifests|https://github.com/Altinity/ClickHouse/pull/2647|||
|Packed parts on CAS disks|https://github.com/Altinity/ClickHouse/pull/2579|||
|Rejoin a decommissioned CAS server root as an empty pool member on restart|https://github.com/Altinity/ClickHouse/pull/2574|26.6.8||
|Live mount-renewal progress and error columns in system.cas_mounts|https://github.com/Altinity/ClickHouse/pull/2513|26.6.8||
|Parallel blob deletion during garbage collection with `cas_gc_io_concurrency`|https://github.com/Altinity/ClickHouse/pull/2351|26.6.8||
|Answer in-part directory probes from manifests, avoiding per-file object-store LIST requests|https://github.com/Altinity/ClickHouse/pull/2440|26.6.8||
|Remove a CAS part with one ref-log record|https://github.com/Altinity/ClickHouse/pull/2452|26.6.8||
|Batch garbage-collection namespace cleanup under a soft time budget|https://github.com/Altinity/ClickHouse/pull/2483|26.6.8||
|EXPORT PARTITION from CAS disks and major garbage-collection round-trip reductions|https://github.com/Altinity/ClickHouse/pull/2300|26.6.4||
|Configure CAS mount lease TTL and renewal period|https://github.com/Altinity/ClickHouse/pull/2307|26.6.4||
|Experimental content-addressed MergeTree storage with shared S3/GCS blobs, fetch-by-relink, garbage collection, FSCK and system tables|https://github.com/Altinity/ClickHouse/pull/2159|26.6.2||
| ---                                                                        |
|:left_right_arrow:  Feature parity in the latest Antalya release  :left_right_arrow:|||       
| ---                                                                        |
|Adaptive Parquet footer reads to reduce storage round trips|https://github.com/Altinity/ClickHouse/pull/2503|26.6.8|https://github.com/ClickHouse/ClickHouse/pull/115686|26.10.2|
|Puffin file format reader|https://github.com/Altinity/ClickHouse/pull/2179|26.6.2|https://github.com/ClickHouse/ClickHouse/pull/109990, https://github.com/ClickHouse/ClickHouse/pull/103936|26.8.1|
|Amazon S3 Tables catalog support|https://github.com/Altinity/ClickHouse/pull/1808|26.3.10|https://github.com/ClickHouse/ClickHouse/pull/103220, https://github.com/ClickHouse/ClickHouse/pull/113505|26.8.1|
|Share S3 client caches per bucket to avoid repeated region discovery|https://github.com/Altinity/ClickHouse/pull/1807|26.3.10|https://github.com/ClickHouse/ClickHouse/pull/96802|26.6.1|
|S3Queue auxiliary ZooKeeper support with keeper_path|https://github.com/Altinity/ClickHouse/pull/2115|25.8.33|https://github.com/ClickHouse/ClickHouse/pull/95203|26.2.1|
|EXECUTE AS for user impersonation|https://github.com/Altinity/ClickHouse/pull/2165|25.8.33|https://github.com/ClickHouse/ClickHouse/pull/70775|25.11.1|
|icebergLocalCluster table function for cluster reads from shared disks|https://github.com/Altinity/ClickHouse/pull/1120|25.8.9|https://github.com/ClickHouse/ClickHouse/pull/93323|26.1.1|
| Hive partition style reads and writes                 |https://github.com/Altinity/ClickHouse/pull/934					 |25.6.5|https://github.com/ClickHouse/ClickHouse/pull/76802|25.8.1|
| Write Parquet enums as byte arrays                        |https://github.com/Altinity/ClickHouse/pull/989           |25.6.5         |https://github.com/ClickHouse/ClickHouse/pull/81090|25.7.1|
| Rendezvous hashing for object-storage filesystem-cache locality                               |https://github.com/Altinity/ClickHouse/pull/709           |25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/82511|25.8.1|
| Faster Hive path parsing with extractKeyValuePairs                          |https://github.com/Altinity/ClickHouse/pull/734           |25.2.2         |https://github.com/ClickHouse/ClickHouse/pull/79067|25.5.1|
| Dynamic cluster autodiscovery                                            |https://github.com/Altinity/ClickHouse/pull/629           |24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/76001|25.3.1|
| Auxiliary ZooKeeper autodiscovery                                           |https://github.com/Altinity/ClickHouse/pull/531           |24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/71911|24.11.1|
| s3Cluster Hive partition pruning, including the old analyzer                               |                        https://github.com/Altinity/ClickHouse/pull/584, https://github.com/Altinity/ClickHouse/pull/703                         |         24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/93284|26.4.1|
| Merge Parquet bloom-filter and min/max evaluation                |https://github.com/Altinity/ClickHouse/pull/590           |24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/71383|25.2.1|
| Parquet integer logical types on the native reader                |https://github.com/Altinity/ClickHouse/pull/589           |24.12.2         |https://github.com/ClickHouse/ClickHouse/pull/72105|25.1.1|


<hr>

*© 2024-2026 Altinity Inc. All rights reserved. Altinity®, Altinity.Cloud®, and Altinity Stable Builds® are registered trademarks of Altinity, Inc. ClickHouse® is a registered trademark of ClickHouse, Inc.*
