-- Tags: no-fasttest
-- Tag no-fasttest: Depends on Minio
--
-- When the `object_storage_cluster_local_read_max_tasks` probe is over the limit, the read stays distributed
-- and reuses the probe's listing. That listing is made with the filter known at planning time, before the
-- optimizer pushes filters into the read. The read must not send more files to the swarm nodes than the same
-- query sends with the probe disabled, whatever shape the filter has.

INSERT INTO FUNCTION s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_a.tsv', 'TSV', 'x UInt32')
SELECT 1
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_b.tsv', 'TSV', 'x UInt32')
SELECT 2
SETTINGS s3_truncate_on_insert = 1;

INSERT INTO FUNCTION s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_c.tsv', 'TSV', 'x UInt32')
SELECT 3
SETTINGS s3_truncate_on_insert = 1;

-- Each query matches two of the three files, so a limit of one file keeps the read distributed.
SELECT 'plain filter, probe disabled';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_*.tsv', 'TSV', 'x UInt32')
WHERE _path IN ('test/05816_object_storage_cluster_local_read_late_a.tsv', 'test/05816_object_storage_cluster_local_read_late_b.tsv')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 0,
    log_comment = '05816_plain_off';

SELECT 'plain filter, probe over the limit';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_*.tsv', 'TSV', 'x UInt32')
WHERE _path IN ('test/05816_object_storage_cluster_local_read_late_a.tsv', 'test/05816_object_storage_cluster_local_read_late_b.tsv')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05816_plain_on';

SELECT 'filter of the outer query, probe disabled';
SELECT count(), sum(x)
FROM (SELECT x, _path FROM s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_*.tsv', 'TSV', 'x UInt32'))
WHERE _path IN ('test/05816_object_storage_cluster_local_read_late_a.tsv', 'test/05816_object_storage_cluster_local_read_late_b.tsv')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 0,
    log_comment = '05816_outer_off';

SELECT 'filter of the outer query, probe over the limit';
SELECT count(), sum(x)
FROM (SELECT x, _path FROM s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_*.tsv', 'TSV', 'x UInt32'))
WHERE _path IN ('test/05816_object_storage_cluster_local_read_late_a.tsv', 'test/05816_object_storage_cluster_local_read_late_b.tsv')
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05816_outer_on';

SELECT 'set from a subquery, probe disabled';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_*.tsv', 'TSV', 'x UInt32')
WHERE _path IN (SELECT arrayJoin(['test/05816_object_storage_cluster_local_read_late_a.tsv', 'test/05816_object_storage_cluster_local_read_late_b.tsv']))
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 0,
    log_comment = '05816_subquery_off';

SELECT 'set from a subquery, probe over the limit';
SELECT count(), sum(x)
FROM s3('http://localhost:11111/test/05816_object_storage_cluster_local_read_late_*.tsv', 'TSV', 'x UInt32')
WHERE _path IN (SELECT arrayJoin(['test/05816_object_storage_cluster_local_read_late_a.tsv', 'test/05816_object_storage_cluster_local_read_late_b.tsv']))
SETTINGS
    object_storage_cluster = 'test_shard_localhost',
    object_storage_cluster_local_read_max_tasks = 1,
    object_storage_cluster_local_read_probe_ms = 5000,
    log_comment = '05816_subquery_on';

SYSTEM FLUSH LOGS query_log;

-- `Shards` is incremented only by the distributed read, so 1 means that both reads were distributed.
-- The task counters count the files sent to the swarm nodes.
WITH runs AS
(
    SELECT
        extract(log_comment, '^05816_([a-z]+)_(?:on|off)$') AS shape,
        extract(log_comment, '_(on|off)$') AS probe,
        ProfileEvents['Shards'] AS shards,
        ProfileEvents['ObjectStorageClusterSentToMatchedReplica'] + ProfileEvents['ObjectStorageClusterSentToNonMatchedReplica'] AS tasks
    FROM system.query_log
    WHERE current_database = currentDatabase()
        AND log_comment IN ('05816_plain_off', '05816_plain_on', '05816_outer_off', '05816_outer_on', '05816_subquery_off', '05816_subquery_on')
        AND type = 'QueryFinish'
)
SELECT
    shape,
    minIf(shards, probe = 'off') AS shards_without_probe,
    minIf(shards, probe = 'on') AS shards_with_probe,
    minIf(tasks, probe = 'on') <= minIf(tasks, probe = 'off') AS probe_sends_no_more_files
FROM runs
GROUP BY shape
ORDER BY shape;
