-- Tags: no-random-settings
-- `input_format_parquet_bloom_filter_coalesce_max_bytes` is new in 26.6.8 Antalya.
-- `compatibility` with 26.6.4, 26.6.2, or 26.6 must restore 0 (one read per bloom filter).

SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.6.8.20001.altinityantalya';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.6.4.20001.altinityantalya';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.6.2.20001.altinityantalya';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');

SET compatibility = '26.6';
SELECT getSetting('input_format_parquet_bloom_filter_coalesce_max_bytes');
