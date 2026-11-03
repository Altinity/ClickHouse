-- Tags: no-ordinary-database, no-replicated-database, no-shared-merge-tree, no-fasttest, no-object-storage, no-s3-storage

SET allow_experimental_unique_key = 1;
SET async_insert = 0;

DROP TABLE IF EXISTS cas_unique_key_mutation_keeps_full;

CREATE TABLE cas_unique_key_mutation_keeps_full
(
    id UInt32,
    value UInt32
)
ENGINE = MergeTree
ORDER BY id
UNIQUE KEY (id)
SETTINGS min_bytes_for_full_part_storage = '1G';

INSERT INTO cas_unique_key_mutation_keeps_full VALUES (1, 10), (2, 20);

SELECT 'before', any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_unique_key_mutation_keeps_full' AND active;

ALTER TABLE cas_unique_key_mutation_keeps_full
    MODIFY COLUMN value UInt64
    SETTINGS mutations_sync = 2;

SELECT 'after', any(part_storage_type)
FROM system.parts
WHERE database = currentDatabase() AND table = 'cas_unique_key_mutation_keeps_full' AND active;

SELECT sum(value) FROM cas_unique_key_mutation_keeps_full;

DROP TABLE cas_unique_key_mutation_keeps_full;
