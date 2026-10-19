-- `ngrambf_v1` must not hash bytes past the end of a string that ends with a truncated UTF-8 sequence.
-- With `index_granularity = 1` the bytes after the first row are the second row, so a buggy index
-- contains the n-gram 'ab\xE4xx' and cannot skip the first granule for the search below.
SET use_skip_indexes = 1;
SET use_query_condition_cache = 0;

DROP TABLE IF EXISTS t_ngram_truncated_utf8;
CREATE TABLE t_ngram_truncated_utf8 (s String, INDEX idx s TYPE ngrambf_v1(3, 256, 2, 0) GRANULARITY 1)
ENGINE = MergeTree ORDER BY tuple() SETTINGS index_granularity = 1, index_granularity_bytes = '10Mi';
INSERT INTO t_ngram_truncated_utf8 SELECT arrayJoin(['ab\xE4', 'xx']);

SELECT count() FROM t_ngram_truncated_utf8 WHERE s LIKE '%ab\xE4xx%';
SELECT trimBoth(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM t_ngram_truncated_utf8 WHERE s LIKE '%ab\xE4xx%')
WHERE explain LIKE '%Granules:%';

DROP TABLE t_ngram_truncated_utf8;
