DROP TABLE IF EXISTS default_conversion_dependencies_wide;
DROP TABLE IF EXISTS default_conversion_dependencies_compact;

CREATE TABLE default_conversion_dependencies_wide
(
    source_before UInt32,
    converted_value Nullable(UInt32),
    source_after UInt32
)
ENGINE = MergeTree
ORDER BY tuple()
SETTINGS min_rows_for_wide_part = 0, min_bytes_for_wide_part = 0;

CREATE TABLE default_conversion_dependencies_compact
AS default_conversion_dependencies_wide
ENGINE = MergeTree
ORDER BY tuple()
SETTINGS min_rows_for_wide_part = 1000000, min_bytes_for_wide_part = 1000000000;

-- Keep the original Nullable data on disk so SELECT must perform the conversion.
SYSTEM STOP MERGES default_conversion_dependencies_wide;
SYSTEM STOP MERGES default_conversion_dependencies_compact;

INSERT INTO default_conversion_dependencies_wide VALUES (10, NULL, 100), (20, 7, 200);
INSERT INTO default_conversion_dependencies_compact VALUES (10, NULL, 100), (20, 7, 200);

ALTER TABLE default_conversion_dependencies_wide
    MODIFY COLUMN converted_value UInt32 DEFAULT source_before + source_after
    SETTINGS mutations_sync = 0, alter_sync = 0;
ALTER TABLE default_conversion_dependencies_compact
    MODIFY COLUMN converted_value UInt32 DEFAULT source_before + source_after
    SETTINGS mutations_sync = 0, alter_sync = 0;

-- Request both DEFAULT inputs explicitly: this tests retention of available inputs.
-- Their types match the part, but skipping them in the conversion block makes
-- evaluation of ifNull(converted_value, source_before + source_after) fail.
SELECT source_before, converted_value, source_after
FROM default_conversion_dependencies_wide
ORDER BY source_before;

SELECT source_before, converted_value, source_after
FROM default_conversion_dependencies_compact
ORDER BY source_before;

DROP TABLE default_conversion_dependencies_wide;
DROP TABLE default_conversion_dependencies_compact;
