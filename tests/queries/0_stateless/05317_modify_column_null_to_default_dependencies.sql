DROP TABLE IF EXISTS default_conversion_dependencies_wide;
DROP TABLE IF EXISTS default_conversion_dependencies_compact;
DROP TABLE IF EXISTS default_conversion_dependencies_chain;

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

-- Neither of the DEFAULT inputs is selected here, so they have to be added to the columns that are read
SELECT converted_value
FROM default_conversion_dependencies_wide
ORDER BY converted_value;

SELECT converted_value
FROM default_conversion_dependencies_compact
ORDER BY converted_value;

-- The PREWHERE step reads source_before, and the conversion of converted_value in the next step still needs it
SELECT source_before, converted_value
FROM default_conversion_dependencies_wide
PREWHERE source_before > 0
WHERE converted_value > 0
ORDER BY source_before;

SELECT source_before, converted_value
FROM default_conversion_dependencies_compact
PREWHERE source_before > 0
WHERE converted_value > 0
ORDER BY source_before;

CREATE TABLE default_conversion_dependencies_chain
(
    source UInt32,
    middle Nullable(UInt32),
    outer_value Nullable(UInt32)
)
ENGINE = MergeTree
ORDER BY tuple();

SYSTEM STOP MERGES default_conversion_dependencies_chain;

INSERT INTO default_conversion_dependencies_chain VALUES (42, NULL, NULL), (1, 5, 9);

ALTER TABLE default_conversion_dependencies_chain
    MODIFY COLUMN middle UInt32 DEFAULT source,
    MODIFY COLUMN outer_value UInt32 DEFAULT middle + 1000
    SETTINGS mutations_sync = 0, alter_sync = 0;

-- Only outer_value is selected so its DEFAULT needs middle, which then needs source for its own DEFAULT
SELECT outer_value
FROM default_conversion_dependencies_chain
ORDER BY outer_value;

DROP TABLE default_conversion_dependencies_wide;
DROP TABLE default_conversion_dependencies_compact;
DROP TABLE default_conversion_dependencies_chain;
