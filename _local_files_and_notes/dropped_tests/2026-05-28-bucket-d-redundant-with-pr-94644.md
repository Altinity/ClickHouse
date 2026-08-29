# Bucket D — redundant with upstream PR #94644

**Context:** During the `__aliasMarker` upstream port (workspace branch
`alias_marker3`), a three-way cross-validation against `upstream/master` and PR
#105690 revealed that 6 of the 17 ported regression tests now pass on pure
master — they no longer reproduce any current regression.

**Cause:** [PR #94644](https://github.com/ClickHouse/ClickHouse/pull/94644)
"Preserve ALIAS column order for distributed reads" landed on upstream
2026-01-22, after the original `__aliasMarker` work began on the 26.3
development branch. #94644 fixes the column-order regression at the
`PlannerJoinTree` / `TableExpressionData` insertion-order level — exactly the
shape these 6 tests were designed to catch — and ships its own test
`03726_distributed_alias_column_order.sql` covering it.

## Shared DDL pattern

All 6 dropped tests share the same nested-alias DDL:

```sql
CREATE TABLE t (x UInt32, a1 UInt32 ALIAS x + 1, a2 UInt32 ALIAS a1 + 1)
ENGINE = MergeTree ORDER BY x;
```

In this pattern both `a1` and `a2` need `x` as input. After #94644 preserves
insertion order, CSE doesn't reorder these expressions on the remote side, so
the initiator/shard header matches as-expected by the time
`addConvertingActions` runs. The bug we were trying to reproduce no longer
manifests.

## What's actually load-bearing on `alias_marker3` after the refactor

The remaining real bugs (not fixed by #94644 and still failing on master) need
different patterns:

- **Shared sub-expression across siblings:** `flag_zero ALIAS toBool(bitTest(f, 0))`,
  `flag_one ALIAS toBool(bitTest(f, 1))` — CSE collapses to a single
  `bitTest(f, ...)` output on the remote, returning fewer columns than the
  initiator expects (`NUMBER_OF_COLUMNS_DOESNT_MATCH`). Covered by the kept
  tests `04279_distributed_alias_planner_column_count` (single-hop and
  multi-hop) and `04280_distributed_alias_column_order` (silent column swap
  with `ORDER BY ... LIMIT`).
- **Multi-hop `Distributed`-over-`Distributed`, `Merge`-over-`Distributed`,
  parallel-replicas follower, and `distributed_product_mode='local'` rewriting
  of `GLOBAL IN`:** covered by the other 4 kept tests (`04281`, `04282`, `04283`).

## Dropped tests — file index in this directory

All preserved with their original SQL (copied from
`feature/antalya-26.3/alias_marker_fixes` tip), using original 26.3 slot
numbers:

| File | Mapped to (now-deleted) slot in alias_marker3 | Status note |
|---|---|---|
| `03930_distributed_alias_swap_planner.sql` | was `04282` on alias_marker3 | Doesn't reproduce on master; #94644 already fixes column order. |
| `03844_distributed_nested_alias_marker.sql` | was `04285` | Doesn't reproduce; #94644 handles the chain. |
| `03845_distributed_global_in_join_alias_chain.sql` | was `04286` | Doesn't reproduce; subquery has one column so column-count divergence dodged. |
| `03846_distributed_global_in_alias_marker_collision.sql` | was `04287` | **Interesting variant.** Two source tables with alias `b` (one is `b ALIAS x`, other `b ALIAS y`). The JOIN resolves on the shard side, so the marker collision is absorbed before the initiator sees it. The *real* collision scenario lives at `04283_distributed_alias_global_in_product_mode_local` (uses `distributed_product_mode='local'` which causes the analyzer to bind both `__table*.x` identifiers to the same alias `foo` on the initiator → `MULTIPLE_EXPRESSIONS_FOR_ALIAS`). |
| `03931_parallel_replicas_alias_swap.sql` | was `04293` | **Interesting variant.** Uses the canonical parallel-replicas-determinism workaround: `GROUP BY x, a1, a2 ORDER BY x` to dedupe non-deterministically distributed rows. Pattern worth recording in case future parallel-replicas tests need it. Underlying scenario doesn't reproduce after #94644. |
| `03932_distributed_alias_strict_name.sql` | was `04294` | Doesn't reproduce; #94644's insertion-order fix handles the reorder + computed column case. |

## How to revisit

If a future change ever undoes #94644's insertion-order guarantee, these tests
will start reproducing again. Resurrect them by copying back from this
directory into `tests/queries/0_stateless/` with fresh slot numbers via
`./tests/queries/0_stateless/add-test`.
