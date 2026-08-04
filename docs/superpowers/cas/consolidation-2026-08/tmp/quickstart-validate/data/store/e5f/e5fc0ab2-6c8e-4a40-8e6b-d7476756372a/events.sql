ATTACH TABLE _ UUID 'ea7aac2e-595c-49d4-86b6-cb691da2c8ef'
(
    `event_date` Date,
    `event_id` UInt64,
    `payload` String
)
ENGINE = MergeTree
ORDER BY event_id
SETTINGS storage_policy = 'cas', index_granularity = 8192
