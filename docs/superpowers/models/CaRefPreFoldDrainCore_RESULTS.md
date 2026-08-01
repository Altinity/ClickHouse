---
description: 'TLC results for the focused CAS ref pre-fold catalog-drain model.'
sidebar_label: 'CAS ref pre-fold drain results'
sidebar_position: 1
slug: '/superpowers/models/cas-ref-prefold-drain-results'
title: 'CAS ref pre-fold drain TLA+ results'
doc_type: 'reference'
---

# `CaRefPreFoldDrainCore` results {#caref-prefolddraincore-results}

`CaRefPreFoldDrainCore.tla` isolates the cross-object ordering between the authoritative adopted
fold seal and the token-CAS namespace catalog. It intentionally contains no physical cleanup:
catalog deletion is the pre-fold barrier, while the perpetual janitor and orphan sweeps own bytes.

The model has two GC actors. Actor A can retain an issued or ambiguous catalog request after actor B
steals the lease. Actor B must read the authoritative parent, resolve its eligible exact catalog CAS,
and take a fresh catalog cut before `DEFER`, ordinary fold adoption, or `REBUILD` adoption. An old
request may still return afterward, but either B consumed the token first or no successor invalidated
the proof under which the old request lands.

## Gate {#gate}

Run from the repository root:

```bash
docs/superpowers/models/run_prefold_drain.sh
```

The committed runner uses one TLC worker for reproducible breadth-first counterexamples. On
2026-08-01, `build/test_task5_prefold_drain_allrows_final_20260801.log` recorded all ten expectations
passed:

| Configuration | Expected | Actual | Generated / distinct states | Depth |
|---|---|---|---:|---:|
| `_sab_fold_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_rebuild_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_defer_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_continue_after_unknown` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 114 / 63 | 6 |
| `_sab_stale_delete_after_successor_hold` | violation: `DeleteUsesCurrentAdoptedProof` | violation: `DeleteUsesCurrentAdoptedProof` | 236 / 122 | 7 |
| `_sab_rebuild_from_unadopted_seal` | violation: `DeleteUsesCurrentAdoptedProof` | violation: `DeleteUsesCurrentAdoptedProof` | 24 / 17 | 4 |
| `_safe` | green | green | 1960 / 734 | 14 |
| `_witness_takeover_converges` | violation: `WITNESS_TAKEOVER_CONVERGES` | violation: `WITNESS_TAKEOVER_CONVERGES` | 1503 / 628 | 12 |

The runner also checks the two-row serial-rescan companion, which owns the full-catalog-token
consequence that a first exact delete invalidates the snapshot for the next candidate:

| Configuration | Expected | Actual | Generated / distinct states | Depth |
|---|---|---|---:|---:|
| `_allrows_sab_skiprescan` | violation: `AllEligibleRowsResolvedBeforeDecision` | violation: `AllEligibleRowsResolvedBeforeDecision` | 6 / 5 | 3 |
| `_allrows_safe` | green | green | 22 / 11 | 6 |

Violation runs stop at their intended shortest counterexample, so their queues are not exhausted.
The honest gate exhausts its queue.

## What each result proves {#what-each-result-proves}

The first three sabotages pin the same barrier at three independent exits. A normal fold,
`REBUILD`, and `DEFER` must not acquire a fresh cut or finish while a ready parent row still has an
unresolved catalog deletion. The `DEFER` sabotage needs the sticky `advancedWithDebt` audit because
it publishes no successor seal of its own.

`_sab_continue_after_unknown` distinguishes an ambiguous storage result from conclusive resolution.
If the exact row is still `Removing`, the actor refreshes the full-catalog token and retries. If the
row is absent, it may continue. A catalog-token conflict with an unchanged exact row is therefore not
completion.

`_sab_stale_delete_after_successor_hold` is the rejected post-adoption-finalizer trace:

1. A reads ready seal S1 and the exact `Removing` catalog token.
2. B steals the lease and, under sabotage, adopts held seal S2 without draining S1.
3. A's old catalog request still matches and deletes the row.

The delete is exact with respect to the catalog but not justified by the current adopted seal, so
`DeleteUsesCurrentAdoptedProof` fails. This is the defect the pre-fold barrier removes.

`_sab_rebuild_from_unadopted_seal` proves that a missing or undecodable authority cannot be replaced
with a seal discovered through enumeration for the purpose of catalog deletion. Honest `REBUILD`
restores authority and returns; a later invocation performs the drain.

The takeover witness reaches the useful honest interleaving rather than merely a sequential delete:
A receives an ambiguous non-landing result, B steals the lease, reads and deletes the same exact
`Removing` row, A resolves absence, and B adopts a successor whose cut omits the row. Both actors
converge without a successor being published over unresolved debt.

`CaRefPreFoldDrainAllRowsCore` is the narrow all-row companion. It starts with two independently
eligible catalog rows; after each exact CAS the actor returns to a complete scan before selecting the
next row. `_allrows_sab_skiprescan` reaches a decision with one row left, while `_allrows_safe`
exhaustively drains both rows before deciding.

## Invariants {#invariants}

- `DrainBeforeDecision` rejects every fold, `REBUILD`, `DEFER`, or fresh-cut continuation that crosses
  unresolved parent debt.
- `DeleteUsesCurrentAdoptedProof` requires a catalog deletion that lands to remain justified by the
  current authoritative matching evidence/no-hold row.
- `ExactCatalogCAS` records that every ordinary deletion consumed its complete observed row and
  full-catalog token.
- `TypeOK` bounds the two leases, one optional catalog-noise write, one catalog deletion, and the
  corresponding seal generations.

TLC logs are written under `tmp/tlc_CaRefPreFoldDrainCore_<config>.log`.
