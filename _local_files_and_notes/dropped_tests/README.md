# Dropped tests register

This directory preserves the rationale and SQL of tests that were dropped from
upstream PRs but might be informative for future contributors. Each entry
documents:

1. **What was dropped** — file names, commit references.
2. **Why it was dropped** — and why the scenario it claimed to test isn't
   load-bearing on the current upstream.
3. **The one interesting non-redundant variant** (if any) — so a future
   contributor doesn't reinvent the same shape.
4. **What's actually load-bearing** — the test (if any) that does cover the
   subsystem.

## Convention

- Dropped SQL files are copied here verbatim (preserving the original 26.3
  slot numbers so they can be located in the source branch).
- A dated rationale Markdown sits next to them with the same prefix.

## Entries

### 2026-05-28: bucket D — alias-marker regression tests redundant with PR #94644

See [`2026-05-28-bucket-d-redundant-with-pr-94644.md`](2026-05-28-bucket-d-redundant-with-pr-94644.md).

Six alias-marker regression tests (slots 03844, 03845, 03846, 03930, 03931,
03932 on `feature/antalya-26.3/alias_marker_fixes`; renumbered to 0428x range
on the upstream port branch `alias_marker3` before being dropped) preserved
in this directory along with their `.reference` files.

### Older entries (pre-2026-05-28)

Files in this directory that don't have a dated Markdown next to them
(`03924_hybrid_unknown_table_exact_schema`, `03925_distributed_alias_column_swap_without_marker`,
`03926_parallel_replicas_dod_alias_column_swap`, `03927_distributed_alias_marker_explicit_column_swap`)
were dropped during earlier iterations of the same `__aliasMarker` work,
without an accompanying note. Rationale for those lives in commit messages on
the source branch.
