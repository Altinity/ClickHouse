ATTACH TABLE _ UUID '99f611b3-c6a6-4fef-9b50-891accf8181d'
(
    `event_date` Date,
    `event_id` UInt64,
    `payload` String
)
ENGINE = MergeTree
PARTITION BY event_date
ORDER BY event_id
SETTINGS storage_policy = 'tiered', index_granularity = 8192
