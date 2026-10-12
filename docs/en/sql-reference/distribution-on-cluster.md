# Task distribution in *Cluster family functions {#task-distribution-in-cluster-family-functions}

## Task distribution algorithm {#task-distribution-algorithm}

Table functions such as `s3Cluster`, `azureBlobStorageCluster`, `hdsfCluster`, `icebergCluster`, and table engines like `S3`, `Azure`, `HDFS`, `Iceberg` with the setting `object_storage_cluster` distribute tasks across all cluster nodes or a subset limited by the `object_storage_max_nodes` setting. This setting limits the number of nodes involved in processing a distributed query, randomly selecting nodes for each query.

A single task corresponds to processing one source file.

For each file, one cluster node is selected as the primary node using a consistent Rendezvous Hashing algorithm. This algorithm guarantees that:
  * The same node is consistently selected as primary for each file, as long as the cluster remains unchanged.
  * When the cluster changes (nodes added or removed), only files assigned to those affected nodes change their primary node assignment.

This improves cache efficiency by minimizing data movement among nodes.

## `lock_object_storage_task_distribution_ms` setting {#lock-object-storage-task-distribution-ms}

Each node begins processing files for which it is the primary node. After completing its assigned files, a node may take tasks from other nodes, either immediately or after waiting for `lock_object_storage_task_distribution_ms` milliseconds if the primary node does not request new files during that interval. The default value of `lock_object_storage_task_distribution_ms` is 500 milliseconds. This setting balances between caching efficiency and workload redistribution when nodes are imbalanced.

## `SYSTEM STOP SWARM MODE` command {#system-stop-swarm-mode}

If a node needs to shut down gracefully, the command `SYSTEM STOP SWARM MODE` prevents the node from receiving new tasks for *Cluster-family queries. The node finishes processing already assigned files before it can safely shut down without errors.

Receiving new tasks can be resumed with the command `SYSTEM START SWARM MODE`.

## Joins {#joins}

[`object_storage_cluster_join_mode`](/operations/settings/settings#object_storage_cluster_join_mode) chooses where a `JOIN` of a `*Cluster` table function, or of a table with `object_storage_cluster` set, is executed, and which table is sent to the nodes. With `allow` and `local` the nodes read the left table and the join runs on the initiator. With `global`, an `INNER`, `LEFT`, or `CROSS` join runs on the nodes and the initiator sends the right table (every table after the first, for `CROSS JOIN`). A `RIGHT` or `FULL` join stays on the initiator.

The same rules for a `Distributed` table, including which side of a `GLOBAL` `RIGHT` join is sent, are described in [Distributed JOIN](/sql-reference/statements/select/join.md#distributed-join).
