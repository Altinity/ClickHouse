-- Tags: no-random-settings
-- `input_format_parquet_bloom_filter_coalesce_max_bytes` is new in 26.8.6 Antalya.
-- `compatibility` with 26.8, 26.7, or 26.6 must restore 0 (one read per bloom filter).

SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.8.6.20001.altinityantalya';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.8';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.7';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.6';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');
