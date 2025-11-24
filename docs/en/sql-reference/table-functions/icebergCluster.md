---
description: 'An extension to the iceberg table function which allows processing files
  from Apache Iceberg in parallel from many nodes in a specified cluster.'
sidebar_label: 'icebergCluster'
sidebar_position: 91
slug: /sql-reference/table-functions/icebergCluster
title: 'icebergCluster'
---

# icebergCluster Table Function

This is an extension to the [iceberg](/sql-reference/table-functions/iceberg.md) table function.

Allows processing files from Apache [Iceberg](https://iceberg.apache.org/) in parallel from many nodes in a specified cluster. On initiator it creates a connection to all nodes in the cluster and dispatches each file dynamically. On the worker node it asks the initiator about the next task to process and processes it. This is repeated until all tasks are finished.

## Syntax {#syntax}

```sql
icebergS3Cluster(cluster_name, url [, NOSIGN | access_key_id, secret_access_key, [session_token]] [,format] [,compression_method])
icebergS3Cluster(cluster_name, named_collection[, option=value [,..]])

icebergAzureCluster(cluster_name, connection_string|storage_account_url, container_name, blobpath, [,account_name], [,account_key] [,format] [,compression_method])
icebergAzureCluster(cluster_name, named_collection[, option=value [,..]])

icebergHDFSCluster(cluster_name, path_to_table, [,format] [,compression_method])
icebergHDFSCluster(cluster_name, named_collection[, option=value [,..]])
```

## Arguments {#arguments}

- `cluster_name` — Name of a cluster that is used to build a set of addresses and connection parameters to remote and local servers.
- Description of all other arguments coincides with description of arguments in equivalent [iceberg](/sql-reference/table-functions/iceberg.md) table function.

**Returned value**

A table with the specified structure for reading data from cluster in the specified Iceberg table.

**Examples**

```sql
SELECT * FROM icebergS3Cluster('cluster_simple', 'http://test.s3.amazonaws.com/clickhouse-bucket/test_table', 'test', 'test')
```

## Virtual Columns {#virtual-columns}

- `_path` — Path to the file. Type: `LowCardinality(String)`.
- `_file` — Name of the file. Type: `LowCardinality(String)`.
- `_size` — Size of the file in bytes. Type: `Nullable(UInt64)`. If the file size is unknown, the value is `NULL`.
- `_time` — Last modified time of the file. Type: `Nullable(DateTime)`. If the time is unknown, the value is `NULL`.
- `_etag` — The etag of the file. Type: `LowCardinality(String)`. If the etag is unknown, the value is `NULL`.

## Altinity Antalya branch

### `icebergLocalCluster` table function

Only in Altinity Antalya branch `icebergLocalCluster` is a function to make distributed cluster request when iceberg data is storend on shared network storage, mounted with local path.
Path must be the same on all replicas.

```sql
icebergLocalCluster(cluster_name, path_to_table, [,format] [,compression_method])
```

### Specify storage type in arguments

Only in Altinity Antalya branch table function `iceberg` can work with all storages.
In this case storage may be specified with named argument `storage_type`.
Possible values are `s3`, `azure`, `hdfs`, `local`:

```sql
icebergCluster(storage_type='s3', cluster_name, url [, NOSIGN | access_key_id, secret_access_key, [session_token]] [,format] [,compression_method])

icebergCluster(storage_type='azure', cluster_name, connection_string|storage_account_url, container_name, blobpath, [,account_name], [,account_key] [,format] [,compression_method])

icebergCluster(storage_type='hdfs', cluster_name, path_to_table, [,format] [,compression_method])

icebergCluster(storage_type='local', cluster_name, path_to_table, [,format] [,compression_method])
```

### Specify storage type in named collection

Only in Altinity Antalya branch `storage_type` may be part of named collection.

```xml
<clickhouse>
    <named_collections>
        <iceberg_conf>
            <url>http://test.s3.amazonaws.com/clickhouse-bucket/</url>
            <access_key_id>test<access_key_id>
            <secret_access_key>test</secret_access_key>
            <format>auto</format>
            <structure>auto</structure>
            <storage_type>s3</storage_type>
        </iceberg_conf>
    </named_collections>
</clickhouse>
```

```sql
icebergCluster(iceberg_conf[, option=value [,..]])
```

Default value for `storage_type` is `s3`.

### `object_storage_cluster` setting.

Only in Altinity Antalya branch alternative syntax for `icebergCluster` table function is `iceberg` function with non-empty setting `object_storage_cluster` with cluster name.

```sql
icebergS3(url [, NOSIGN | access_key_id, secret_access_key, [session_token]] [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

icebergAzure(connection_string|storage_account_url, container_name, blobpath, [,account_name], [,account_key] [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

icebergHDSF(path_to_table, [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

icebergLocal(path_to_table, [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

icebergS3(option=value [,..]]) SETTINGS object_storage_cluster='cluster_name'

iceberg(storage_type='s3', url [, NOSIGN | access_key_id, secret_access_key, [session_token]] [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

iceberg(storage_type='azure', connection_string|storage_account_url, container_name, blobpath, [,account_name], [,account_key] [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

iceberg(storage_type='hdfs', path_to_table, [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

iceberg(storage_type='local', path_to_table, [,format] [,compression_method]) SETTINGS object_storage_cluster='cluster_name'

iceberg(iceberg_conf[, option=value [,..]]) SETTINGS object_storage_cluster='cluster_name'
```

**See Also**

- [Iceberg engine](/engines/table-engines/integrations/iceberg.md)
- [Iceberg table function](sql-reference/table-functions/iceberg.md)
