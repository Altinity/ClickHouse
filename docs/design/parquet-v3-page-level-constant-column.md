# Parquet v3: per-subgroup (page-level) constant-column detection

## Motivation

The current constant-column optimization (`detectConstantColumn`) fires only when a whole
Parquet **column chunk** is single-valued (footer `min == max`, no nulls). Data that is sorted or
clustered on a column is often constant over long **runs of pages** without the whole row group
being constant, so chunk-level detection misses it.

Parquet's **Column Index** stores per-page `min_values` / `max_values` / `null_pages` /
`null_counts`. The v3 reader already loads and parses it (`applyColumnIndex`) for predicate
push-down, so per-page "is this page constant?" is available at zero extra I/O. This lets us mark a
column constant for the row range of an individual **row subgroup** (the unit that becomes one
output `Chunk`) and materialize it as a `ColumnConst`, skipping the covered pages' reads and decode.

## Tiers (both kept)

| Tier | Source | Always present? | Exact flag? | Granularity |
|------|--------|-----------------|-------------|-------------|
| 1 (existing) | footer `ColumnMetaData.statistics` | yes | yes (`is_min_value_exact`) | whole chunk |
| 2 (this doc) | Column Index (per page) | no (optional) | no | per subgroup |

Tier 1 stays the always-on baseline and the **only** safe detector for `BYTE_ARRAY` /
`FIXED_LEN_BYTE_ARRAY` (Column Index has no per-page exactness flag; 16-byte truncation can make two
distinct strings compare equal). Tier 2 is opportunistic: only for fixed-width numeric/date/time,
only when the Column Index is already loaded, only for chunks tier 1 did not already mark constant.

## Key decision: do NOT change subgroup sizing

Aligning subgroups to page boundaries would fragment the block stream into many tiny chunks
(pages are far smaller than a row group), and a subgroup carries all columns so its size is bounded
by the non-constant columns anyway. Instead, keep subgroup boundaries exactly as today and do
**per-subgroup, per-column** detection: a column is constant for a subgroup iff every Column-Index
page overlapping the subgroup's row range is constant with the *same* value (or every such page is
`null_pages`). Chunk count is unchanged; we just catch subgroups that sit inside a constant run.

## Phases

- **Phase 0** — retain per-page constant info. `applyColumnIndex` currently discards the parsed
  `parq::ColumnIndex`. Keep a compact per-page summary on `ColumnChunk` (value + `is_const` +
  `all_null`), plus the page→`first_row_index` map already in the Offset Index. Populate only for
  eligible types.
- **Phase 1** — `detectConstantSubchunk(column, column_info, [start_row, end_row))`: scan the pages
  overlapping the range; return constant + value when all are `is_const` and share one value;
  all-null when all are `null_pages`.
- **Phase 2** — call it in `intersectColumnIndexResultsAndInitSubgroups` for each subgroup /
  primitive column that tier 1 didn't already mark constant; set `subchunk.is_constant` /
  `is_all_null` / `constant_value` (the same fields `decodePrimitiveColumn` propagates).
  `formOutputColumn` needs no change (already emits `ColumnConst`).
- **Phase 3** — skip work for constant subchunks: `decodePrimitiveColumn` skips decode on
  `subchunk.is_constant`; `determinePagesToPrefetch` skips fetching a page only when it is constant
  in **every** subgroup that overlaps it.
- **Phase 4** — stateless tests (page-run constant, all-null-per-page, byte-array negative/truncation
  guard, `GROUP BY` correctness), with a new `ParquetConstantColumnSubchunks` ProfileEvent to prove
  tier 2 fired.
- **Phase 5** — ProfileEvents comparison on clustered data: expect further `S3GetObject` /
  `ParquetFetchWaitTimeMicroseconds` drops with chunk count unchanged.

## Optional force-load

By default tier 2 only uses the Column Index when it is already loaded (columns with a predicate
push-down). `input_format_parquet_use_column_index_for_constant_columns` (default off) extends it:
the Column Index (+ Offset Index) is force-loaded for eligible read columns that have no predicate,
so tier 2 can also fire on them. Cost is a small extra read of the (tiny, tail-contiguous, coalesced)
index; worthwhile mainly for sorted / low-cardinality columns. `applyColumnIndex` records per-page
constant info but skips predicate pruning when the column has no condition. A future `auto` mode
could gate this on footer signals (row-group `sorting_columns`, low compressed-bytes-per-value,
dictionary encoding stats) instead of an all-or-nothing switch.

## Cast safety (future-proofing)

The optimization only fires when the stats decoder needs no value-transforming conversion
(`SchemaConverter` sets `allow_stats` accordingly), so today `input_type == output_type` for every
constant column and no cast is applied. To keep the constant path correct if `allow_stats` is ever
generalized to allow transforming casts, `formOutputColumn` builds the single-value constant in
`input_type` and runs the same `castColumn` the per-row decode uses when `needs_cast` is set - a
no-op today, O(1) on the `ColumnConst`, and it preserves const-ness. The all-null constant is
synthesized directly in the output domain (Null / output default), so it is not cast. The mixed fill
already goes through the normal `formOutputColumn` cast, so it is future-safe too. This does not
touch the `allow_stats` decision itself.

## Guardrails

- All types, including `BYTE_ARRAY` / `FIXED_LEN_BYTE_ARRAY` strings. No truncation guard is needed
  for the `min == max` case: statistics/Column-Index bounds are always valid
  (`min <= every value <= max`) and truncation only widens them, so `min == max` proves a single
  exact value (a truncated or multi-valued page yields `min < max`). This holds at both the chunk
  level (tier 1) and per page (tier 2), so neither needs the `is_*_value_exact` flag.
- Gate on the existing `input_format_parquet_use_constant_column_optimization` setting; the
  force-load above is additionally gated by
  `input_format_parquet_use_column_index_for_constant_columns`.
- Partial-page subgroup boundaries are fine: a partial overlap of a constant page still yields that
  value, as long as every overlapping page is constant with the shared value.

## Mixed-topology fill (Approach B) — implemented, default off

`input_format_parquet_fill_constant_pages` handles constant runs *shorter* than / straddling a
subgroup: `fillConstantPagesAndDecodeRest` fills single-value pages from the Column Index and decodes
only the varying ones, and `determinePagesToPrefetch` skips prefetching the filled pages (a page
shared with a subgroup that decodes it normally is still fetched — `willFillConstantPages` is
deterministic so both paths agree). The fill walks the rows that pass the filter (`row_subgroup.filter`), so it works under PREWHERE /
row-level filters and page-pruning predicates too - it produces exactly `rows_pass` values for any
filter, and because the decision no longer depends on `rows_pass` it is identical at prefetch time
and decode time (so the prefetch-skip can never drop a page the decode needs). All-null pages are
filled (nulls via the compact values + null map + `expand` path, or the output default under
`null_as_default`); a subgroup with an all-null page falls back only when the output can represent
neither null nor a default. The one remaining gate is `needs_cast`: the fill writes the Column Index
value into the `decoded_type` column, valid only when no post-decode cast applies (resolving whether
`decodeField` yields the decoded or the output value domain is build-gated). Experimental, off by
default; needs a build + correctness tests before it can be trusted.
