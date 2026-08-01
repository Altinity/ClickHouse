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
the proof under which the old request lands. The model now carries that fresh cut's full-catalog
token/value into the ref-plan boundary, without importing any fold lifecycle from
`CaRefDeltaIntakeCore`.

## Gate {#gate}

Run from the repository root:

```bash
docs/superpowers/models/run_prefold_drain.sh
```

The committed runner uses one TLC worker for reproducible breadth-first counterexamples. On
2026-08-01, `build/test_task5_prefold_cut_provenance_fix1_full_20260801.log` recorded all fourteen
expectations passed:

| Configuration | Expected | Actual | Generated / distinct states | Depth |
|---|---|---|---:|---:|
| `_sab_fold_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_rebuild_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_defer_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_continue_after_unknown` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 114 / 64 | 6 |
| `_sab_stale_delete_after_successor_hold` | violation: `DeleteUsesCurrentAdoptedProof` | violation: `DeleteUsesCurrentAdoptedProof` | 240 / 127 | 7 |
| `_sab_rebuild_from_unadopted_seal` | violation: `DeleteUsesCurrentAdoptedProof` | violation: `DeleteUsesCurrentAdoptedProof` | 24 / 17 | 4 |
| `_sab_intake_uses_predrain_cut` | violation: `IntakeConsumesFreshPostDrainCut` | violation: `IntakeConsumesFreshPostDrainCut` | 209 / 111 | 7 |
| `_sab_intake_uses_stale_token` | violation: `IntakeConsumesFreshPostDrainCut` | violation: `IntakeConsumesFreshPostDrainCut` | 209 / 111 | 7 |
| `_safe` | green | green | 2272 / 874 | 14 |
| `_witness_takeover_converges` | violation: `WITNESS_TAKEOVER_CONVERGES` | violation: `WITNESS_TAKEOVER_CONVERGES` | 1733 / 737 | 12 |
| `_witness_drained_row_absent_from_intake` | violation: `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE` | violation: `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE` | 208 / 110 | 7 |

The runner also checks the two-row serial-rescan companion, which owns the full-catalog-token
consequence that a first exact delete invalidates the snapshot for the next candidate:

| Configuration | Expected | Actual | Generated / distinct states | Depth |
|---|---|---|---:|---:|
| `_allrows_sab_skiprescan` | violation: `AllEligibleRowsResolvedBeforeDecision` | violation: `AllEligibleRowsResolvedBeforeDecision` | 10 / 8 | 3 |
| `_allrows_sab_nonexactdelete` | violation: `ExactCatalogCAS` | violation: `ExactCatalogCAS` | 21 / 13 | 4 |
| `_allrows_safe` | green | green | 80 / 29 | 6 |

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

`_sab_intake_uses_predrain_cut` isolates the composition seam after the lifecycle work is already
correct. A observes `Removing` at catalog token 1, exact deletion advances the catalog, and
`TakeFreshCut` records `absent` at token 2. The sabotage then gives ref-plan intake the earlier
`Removing`/token-1 observation, which would admit the drained life; only
`IntakeConsumesFreshPostDrainCut` fails. This is independent of
`CaRefDeltaIntakeCore_sab_adoptbeforecommit`, whose `NoMissedFold` failure says nothing about catalog
cut provenance.

`_sab_intake_uses_stale_token` independently pins the token half of that provenance contract at the
actual `AdoptFromCut` plan/adoption seam. Adoption and the plan consume the honest fresh `absent` row,
so `adoptedRow = "none"` and `plan_has_life = FALSE`, but intake carries the earlier token 1 instead
of the fresh token 2. Row equality therefore remains true and only the token-equality conjunct of
`IntakeConsumesFreshPostDrainCut` can make the run red.

`_witness_drained_row_absent_from_intake` reaches the honest dual: the exact deletion advances token
1 to 2, the consumed cut is `absent` at token 2, and `plan_has_life = FALSE`. The witness makes the
composition control non-vacuous and proves the drained row is absent from both the consumed cut and
the one-row plan projection.

`CaRefPreFoldDrainAllRowsCore` is the narrow all-row companion. It starts with two independently
eligible catalog rows; a rejected or non-landing ambiguous response, and every local or external
resolution, return to a complete scan before selecting the next row. `_allrows_sab_skiprescan` reaches a decision with one row left;
`_allrows_sab_nonexactdelete` accepts a stale token and turns the sticky exactness consequence red;
and `_allrows_safe` exhaustively drains both rows before deciding. `CompletionDrained` defines that
decision precisely: `phase = "done" => remaining = {}`.

## Invariants {#invariants}

- `DrainBeforeDecision` rejects every fold, `REBUILD`, `DEFER`, or fresh-cut continuation that crosses
  unresolved parent debt.
- `DeleteUsesCurrentAdoptedProof` requires a catalog deletion that lands to remain justified by the
  current authoritative matching evidence/no-hold row.
- `ExactCatalogCAS` records that every ordinary deletion consumed its complete observed row and
  full-catalog token. Its falsifiable red control is `_allrows_sab_nonexactdelete`, which makes the
  stale-token consequence sticky rather than deriving it from the honest action's own guard.
- `IntakeConsumesFreshPostDrainCut` requires the ref-plan boundary to consume the exact token/value
  pair exported by `TakeFreshCut`. `_sab_intake_uses_predrain_cut` makes the value and token stale,
  while `_sab_intake_uses_stale_token` holds the fresh row value constant and changes only the token;
  both are red even though the drain and fresh-cut ordering themselves are honest.
- `CompletionDrained` requires that the all-row companion can decide only after its remaining eligible
  set is empty.
- `TypeOK` bounds the two leases, one optional catalog-noise write, one catalog deletion, and the
  corresponding seal generations.

The provenance TDD evidence is preserved in
`build/test_task5_prefold_cut_provenance_red_20260801.log`,
`build/test_task5_prefold_cut_provenance_safe_20260801.log`, and
`build/test_task5_prefold_cut_provenance_witness_clean_20260801.log`. The independent token-only RED
is in `build/test_task5_prefold_cut_provenance_fix1_stale_token_red_20260801.log`; the clean fourteen-
case gate is in `build/test_task5_prefold_cut_provenance_fix1_full_20260801.log`. The runner also
writes per-config TLC logs under `tmp/tlc_CaRefPreFoldDrainCore_<config>.log`.
