-- Regression coverage for nested `ALIAS` columns across `remote` -> `Distributed` -> `MergeTree`.

DROP TABLE IF EXISTS test_alias_pr_second_hop_dist;
DROP TABLE IF EXISTS test_alias_pr_second_hop_local;

CREATE TABLE test_alias_pr_second_hop_local
(
    dt DateTime64(3),
    base String,
    alias_base_0 String ALIAS base,
    alias_base_1 String ALIAS base
)
ENGINE = MergeTree()
ORDER BY dt;

INSERT INTO test_alias_pr_second_hop_local VALUES
    ('1999-03-29T01:15:33', 'x'),
    ('1999-03-29T01:15:34', 'y');

CREATE TABLE test_alias_pr_second_hop_dist AS test_alias_pr_second_hop_local
ENGINE = Distributed(test_cluster_one_shard_three_replicas_localhost, currentDatabase(), test_alias_pr_second_hop_local);

SELECT 'ast_second_hop';
SELECT dt, alias_base_0, alias_base_1
FROM remote('127.0.0.2', currentDatabase(), test_alias_pr_second_hop_dist)
ORDER BY dt
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, serialize_query_plan = 0;

SELECT 'plan_second_hop';
SELECT dt, alias_base_0, alias_base_1
FROM remote('127.0.0.2', currentDatabase(), test_alias_pr_second_hop_dist)
ORDER BY dt
SETTINGS enable_analyzer = 1, enable_alias_marker = 1, serialize_query_plan = 1;

DROP TABLE test_alias_pr_second_hop_dist;
DROP TABLE test_alias_pr_second_hop_local;
