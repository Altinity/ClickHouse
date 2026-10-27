-- Tags: no-fasttest, no-random-merge-tree-settings, no-replicated-database, no-shared-merge-tree

DROP TABLE IF EXISTS cas_mutation_source_plain SYNC;
DROP TABLE IF EXISTS cas_mutation_source_replicated SYNC;

CREATE TABLE cas_mutation_source_plain
(
    id UInt64,
    indexed_value UInt64,
    projected_value UInt64,
    payload UInt64,
    INDEX value_idx indexed_value TYPE minmax GRANULARITY 1,
    PROJECTION p (SELECT id, projected_value ORDER BY projected_value)
)
ENGINE = MergeTree
ORDER BY id
SETTINGS min_bytes_for_wide_part = 0,
    min_rows_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0,
    min_rows_for_full_part_storage = 0,
    packed_skip_index_max_bytes = 0,
    index_granularity = 1;

INSERT INTO cas_mutation_source_plain
SELECT number, intDiv(number, 4), (number * 7) % 16, number + 100
FROM numbers(16);

SELECT 'plain_before', count(), any(part_type), any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_mutation_source_plain' AND active;

ALTER TABLE cas_mutation_source_plain MODIFY SETTING
    min_bytes_for_full_part_storage = '1G',
    min_rows_for_full_part_storage = 1000000000;

ALTER TABLE cas_mutation_source_plain
    UPDATE payload = payload + 1 WHERE id % 2 = 0
    SETTINGS mutations_sync = 2;

SELECT 'plain_data', sum(payload) FROM cas_mutation_source_plain;

SELECT 'plain_after', count(), any(part_type), any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_mutation_source_plain' AND active;

SELECT 'plain_check';
CHECK TABLE cas_mutation_source_plain SETTINGS check_query_single_value_result = 1;

SELECT 'plain_projection', count(), sum(projected_value)
FROM cas_mutation_source_plain
WHERE projected_value IN (3, 7)
SETTINGS optimize_use_projections = 1,
    force_optimize_projection = 1,
    force_optimize_projection_name = 'p';

SELECT 'plain_index', count()
FROM cas_mutation_source_plain
WHERE indexed_value = 2
SETTINGS optimize_use_projections = 0,
    force_data_skipping_indices = 'value_idx';

CREATE TABLE cas_mutation_source_replicated
(
    id UInt64,
    indexed_value UInt64,
    projected_value UInt64,
    payload UInt64,
    INDEX value_idx indexed_value TYPE minmax GRANULARITY 1,
    PROJECTION p (SELECT id, projected_value ORDER BY projected_value)
)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/05301_cas_mutation_preserves_source_storage', 'r1')
ORDER BY id
SETTINGS min_bytes_for_wide_part = 0,
    min_rows_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0,
    min_rows_for_full_part_storage = 0,
    packed_skip_index_max_bytes = 0,
    index_granularity = 1;

INSERT INTO cas_mutation_source_replicated
SELECT number, intDiv(number, 4), (number * 7) % 16, number + 100
FROM numbers(16);

SELECT 'replicated_before', count(), any(part_type), any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_mutation_source_replicated' AND active;

ALTER TABLE cas_mutation_source_replicated MODIFY SETTING
    min_bytes_for_full_part_storage = '1G',
    min_rows_for_full_part_storage = 1000000000;

ALTER TABLE cas_mutation_source_replicated
    UPDATE payload = payload + 1 WHERE id % 2 = 0
    SETTINGS mutations_sync = 2;

SELECT 'replicated_data', sum(payload) FROM cas_mutation_source_replicated;

SELECT 'replicated_after', count(), any(part_type), any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_mutation_source_replicated' AND active;

SELECT 'replicated_check';
CHECK TABLE cas_mutation_source_replicated SETTINGS check_query_single_value_result = 1;

SELECT 'replicated_projection', count(), sum(projected_value)
FROM cas_mutation_source_replicated
WHERE projected_value IN (3, 7)
SETTINGS optimize_use_projections = 1,
    force_optimize_projection = 1,
    force_optimize_projection_name = 'p';

SELECT 'replicated_index', count()
FROM cas_mutation_source_replicated
WHERE indexed_value = 2
SETTINGS optimize_use_projections = 0,
    force_data_skipping_indices = 'value_idx';

DROP TABLE cas_mutation_source_replicated SYNC;
DROP TABLE cas_mutation_source_plain SYNC;
