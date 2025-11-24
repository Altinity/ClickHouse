# Task distribution in *Cluster family functions

Table functions `s3Cluster`, `azureBlobStorageCluster`, `hdsfCluster`, `icebergCluster`, etc., as well as table engines `S3`, `Azure`, `HDFS`, `Iceberg` with setting `object_storage_cluster` distribute tasks between all cluster nodes, or only between `object_storage_max_nodes` cluster nodes. Setting `object_storage_max_nodes` limits number of nodes to process distributed query. In this case random nodes are choosed for each query.

Single task is a single source file.

For each file one of cluster nodes selected as primary node. Primary node selected with consistence Rendezvous Hashing algorythm, this algorythm guarantees that for each file sthe same node selected as primary when cluster not changed, and when cluster is changed only files from deleted nodes or with new nodes as primary are affected. This incerasy cache efficiency.
Each node starts to process files for which this node is primary. When node processes all those files it can take some files from other nodes. Node gets file immediately or only when primary node does not ask for new files in `lock_object_storage_task_distribution_ms` milliseconds. Settiing `lock_object_storage_task_distribution_ms` has default value in 500 milliseconds, and can be used to achieve balance between caching and processing files when some nodes are overloaded.

If node must be shutdowned in some time, command `SYSTEM STOP SWARM MODE` can be used to stop getting new tasks for `*Cluster`-family queries. In this case node stop getting new files, but processes alrready started files. When it processes all files, node can be shutdowned withou any error on initiator.

Getting new tasks can be reenabled with `SYSTEM START SWARM MODE` command.
