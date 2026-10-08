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
complete the hot stream LIST, take ONE fresh authoritative catalog cut, and build the ref-walk plan
before `DEFER`, ordinary fold adoption, or `REBUILD` adoption. An old
request may still return afterward, but either B consumed the token first or no successor invalidated
the proof under which the old request lands. The model carries opaque life identity separately from
catalog lifecycle state through drain observation, cut and intake; ref-plan membership is a set of
life ids rather than an inference from `Live`/`Removing`. It carries the fresh cut's full-catalog
token/value into the ref-plan boundary without importing any fold lifecycle from
`CaRefDeltaIntakeCore`. Same-name rebirth advances a fresh successor through catalog `Creating`,
catalog `Live`, first stream `PUT` and legal `Removing`, while the predecessor stream remains
LIST-visible inert debris.

## Gate {#gate}

Run from the repository root:

```bash
docs/superpowers/models/run_prefold_drain.sh
```

The committed runner uses one TLC worker for reproducible breadth-first counterexamples. On
2026-08-01, `build/task5_review1_prefold_full_gate.log` recorded all eighteen expectations passed:

| Configuration | Expected | Actual | Generated / distinct states | Depth |
|---|---|---|---:|---:|
| `_sab_fold_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_rebuild_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_defer_bypasses_drain` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 54 / 32 | 5 |
| `_sab_continue_after_unknown` | violation: `DrainBeforeDecision` | violation: `DrainBeforeDecision` | 116 / 66 | 6 |
| `_sab_stale_delete_after_successor_hold` | violation: `DeleteUsesCurrentAdoptedProof` | violation: `DeleteUsesCurrentAdoptedProof` | 255 / 138 | 7 |
| `_sab_rebuild_from_unadopted_seal` | violation: `DeleteUsesCurrentAdoptedProof` | violation: `DeleteUsesCurrentAdoptedProof` | 24 / 17 | 4 |
| `_sab_intake_uses_predrain_cut` | violation: `IntakeConsumesFreshPostDrainCut` | violation: `IntakeConsumesFreshPostDrainCut` | 406 / 210 | 8 |
| `_sab_intake_uses_stale_token` | violation: `IntakeConsumesFreshPostDrainCut` | violation: `IntakeConsumesFreshPostDrainCut` | 406 / 210 | 8 |
| `_sab_cut_before_list` | violation: `ListedCurrentLifeIsAdmitted` | violation: `ListedCurrentLifeIsAdmitted` | 2895 / 1415 | 11 |
| `_sab_absent_listed_defers` | violation: `DeadListedPredecessorIsInert` | violation: `DeadListedPredecessorIsInert` | 406 / 210 | 8 |
| `_sab_predecessor_deletes_successor` | violation: `PredecessorProofCannotDeleteSuccessorRemoving` | violation: `PredecessorProofCannotDeleteSuccessorRemoving` | 7847 / 3499 | 13 |
| `_safe` | green | green | 91992 / 35506 | 24 |
| `_witness_takeover_converges` | violation: `WITNESS_TAKEOVER_CONVERGES` | violation: `WITNESS_TAKEOVER_CONVERGES` | 12188 / 5360 | 14 |
| `_witness_drained_row_absent_from_intake` | violation: `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE` | violation: `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE` | 735 / 371 | 9 |
| `_witness_rebirth_with_retained_debris_adopts` | violation: `WITNESS_REBIRTH_WITH_RETAINED_DEBRIS_ADOPTS` | violation: `WITNESS_REBIRTH_WITH_RETAINED_DEBRIS_ADOPTS` | 37631 / 15556 | 17 |

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
so `adoptedRow = "none"` and `plan_lives = {}`, but intake carries the earlier token 1 instead
of the fresh token 2. Row equality therefore remains true and only the token-equality conjunct of
`IntakeConsumesFreshPostDrainCut` can make the run red.

`_sab_cut_before_list` executes the old order through its harmful consequence rather than stopping at
the early cut. After an exact predecessor delete, it records an absent cut; the same name is reborn
through `Creating`, `Live` and stream `PUT`; the completed LIST returns predecessor and successor;
then plan intake consumes the stale absent cut. `ListedCurrentLifeIsAdmitted` fails because the
LIST-visible current successor is omitted and classified with dead debris. The additional
`FreshCutFollowsCompletedHotList` provenance invariant is checked at the same plan-consumption seam,
not used as the primary counterexample.

`_sab_absent_listed_defers` keeps the honest drain, completed LIST and later cut, but restores the old
"absent listed id is unknown" classifier. The retained predecessor is counted and excluded from the
plan, yet the sabotage sets `defer_for_dead_debris`; only `DeadListedPredecessorIsInert` fails.

`_sab_predecessor_deletes_successor` removes opaque identity from the exact-delete match. Actor A
retains predecessor proof and observation while actor B deletes that predecessor; the same name is
reborn and legally reaches successor `Removing`; the sabotaged old request then deletes the successor.
`PredecessorProofCannotDeleteSuccessorRemoving` fails. The honest action matches life id, lifecycle
state and full-catalog token, so this transition is unavailable in the safe configuration.

`_witness_rebirth_with_retained_debris_adopts` reaches the constructive dual and the stale-request
interleaving in one trace. A issues against predecessor `Removing`; B deletes that exact predecessor;
A's request returns a token/identity conflict; the same name advances through `Creating`, `Live`,
stream `PUT` and legal successor `Removing`; and B's completed LIST returns both keys. The sole later
cut carries `{successor_life, Removing}`; `buildRefWalkPlan` admits exactly `successor_life`, counts
one predecessor debris id, sets no debris-DEFER bit, and `AdoptFromCut` publishes the successor. Thus
`Removing` is not an identity, and stale predecessor proof cannot target or suppress the successor.

`_witness_drained_row_absent_from_intake` reaches the honest dual: the exact deletion advances token
1 to 2, the consumed cut is `absent` at token 2, and `plan_lives = {}`. The witness makes the
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
- `FreshCutFollowsCompletedHotList` requires every nonzero cut token to have a completed hot LIST in
  the same actor's immutable intake record and to carry post-LIST provenance. It is an additional
  assertion at intake; the consequential `_sab_cut_before_list` control is primarily red on
  `ListedCurrentLifeIsAdmitted`.
- `DeadListedPredecessorIsInert` requires a retained predecessor absent from the later cut to be
  counted once, excluded from the life-id plan and never used as a DEFER reason.
- `ListedCurrentLifeIsAdmitted` rejects a stale cut which omits a LIST-visible current successor.
- `SuccessorRemovingIsAdmitted` requires `{successor_life, Removing}` to produce the exact singleton
  life-id plan `{successor_life}`, never the predecessor.
- `PredecessorProofCannotDeleteSuccessorRemoving` is the sticky exact-delete consequence controlled
  by `_sab_predecessor_deletes_successor`.
- `IntakeConsumesFreshPostDrainCut` requires the ref-plan boundary to consume the exact token/value
  pair exported by `TakeFreshCut`, including both life id and lifecycle state. Its plan projection is
  exactly the walkable life-id set. `_sab_intake_uses_predrain_cut` makes the value and token stale,
  while `_sab_intake_uses_stale_token` holds the fresh row value constant and changes only the token;
  both are red even though the drain and fresh-cut ordering themselves are honest. Its plan projection
  admits `Live`/`Removing` directly from the cut, independent of whether LIST omitted the successor.
- `CompletionDrained` requires that the all-row companion can decide only after its remaining eligible
  set is empty.
- `TypeOK` bounds the two leases, one optional catalog-noise write, one catalog deletion, and the
  corresponding seal generations.

The review-fix TDD evidence is preserved in
`build/task5_review1_identity_state_red_targeted.log` (state-only admission RED),
`build/task5_review1_identity_state_green_attempt3.log` (the first identity GREEN),
`build/task5_review1_cut_before_list_semantic_red_attempt2.log` (consequential stale-cut RED under
the remaining safety invariants), `build/task5_review1_predecessor_deletes_successor_red_attempt2.log`
(identity-blind delete RED under the remaining safety invariants),
`build/task5_review1_rebirth_removing_witness_attempt3.log` (stale conflict plus successor
`Removing` adoption), and `build/task5_review1_prefold_safe_attempt2.log` (final exhaustive GREEN).
The clean eighteen-case gate is `build/task5_review1_prefold_full_gate.log`. Earlier post-LIST and
cut-to-intake provenance evidence remains recorded in the prior report history. The runner also writes
per-config TLC logs under `tmp/tlc_CaRefPreFoldDrainCore_<config>.log`.
