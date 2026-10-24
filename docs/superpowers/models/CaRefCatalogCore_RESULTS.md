# `CaRefCatalogCore` TLA+ gate results {#carefcatalogcore-results}

Model: `CaRefCatalogCore.tla`. This is the local namespace-lifecycle model for the ref-chain design.
It proves creation and reconciliation safety, fresh opaque life identity, bounded catalog churn, and
the predicates under which an exact `Removing -> absent` catalog CAS is locally authorized. It does
not prove when a GC invocation may perform that CAS relative to another invocation's adopted seal;
`CaRefPreFoldDrainCore.tla` is the single owner of that cross-object A/B ordering.

Runner: `./run_refcatalog.sh`, TLC 2.19 with one worker. The table records the run on 2026-08-01 from
`build/test_task5_prefold_refcatalog_20260801.log` and its individual `tmp/tlc_CaRefCatalogCore_*.log` files.
All 16 expectations passed.

## Results {#carefcatalogcore-results-table}

For a violation, the state counts and depth describe TLC's shortest counterexample before its queue
is exhausted. Green configurations exhaust the complete graph.

| Configuration | Expected result | Generated / distinct | Depth |
|---|---|---:|---:|
| `_sab_janitoreatsnewborn` | `INV_NEWBORN_SAFE` violated | 34 / 18 | 5 |
| `_sab_zombiegolive` | `INV_NEWBORN_SAFE` violated | 178 / 66 | 8 |
| `_sab_reconcilelivecreator` | `INV_RECONCILE_SAFE` violated | 41 / 21 | 5 |
| `_sab_reconcilestaletoken` | `INV_RECONCILE_SAFE` violated | 102 / 47 | 7 |
| `_sab_deletewithoutevidence` | `INV_REMOVAL_DELETE_PROVED` violated | 79 / 37 | 7 |
| `_sab_deletewithforeignevidence` | `INV_REMOVAL_DELETE_PROVED` violated | 193 / 74 | 8 |
| `_sab_deleteunderhold` | `INV_REMOVAL_DELETE_PROVED` violated | 257 / 80 | 9 |
| `_sab_deletewithoutexactobservation` | `INV_REMOVAL_DELETE_PROVED` violated | 73 / 36 | 7 |
| `_sab_sameincarnationrebirth` | `INV_NO_ALIAS` violated | 160 / 59 | 8 |
| `_sab_floorretainsdeadname` | `INV_BOUNDED_CATALOG` violated | 146 / 58 | 8 |
| `_finding_briefreconcileinv` | `INV_RECONCILE_SAFE_BRIEF` violated | 6 / 4 | 3 |
| `_safe` | green | 2924 / 583 | 19 |
| `_churn` | green | 17110 / 3018 | 27 |
| `_witness_churn3` | `WITNESS_CHURN` violated | 11992 / 2449 | 23 |
| `_witness_aliasremnant` | `WITNESS_ALIAS_REMNANT` violated | 475 / 135 | 11 |
| `_witness_orphaneaten` | `WITNESS_ORPHAN_EATEN` violated | 186 / 73 | 8 |

## Local deletion proof {#carefcatalogcore-local-deletion-proof}

`INV_REMOVAL_DELETE_PROVED` records the proof attached to the most recent catalog deletion. An honest
deletion requires all three facts:

- the authoritative adopted life row contains positive terminal cleanup evidence for this exact
  opaque life id;
- that same adopted row carries no durable hold;
- the mutator acts on the complete exact `Removing` row it observed, so the full-catalog token CAS
  cannot delete a replacement row.

The four dedicated sabotages make each part executable rather than implicit:

- without evidence, TLC reaches `EntryDelete` after merely observing the row;
- foreign evidence from an earlier life does not prove the current life complete;
- evidence does not override a durable hold;
- omitting the exact observation lets deletion proceed without the row-token precondition.

All four violate `INV_REMOVAL_DELETE_PROVED`; the honest lifecycle and churn configurations remain
green. The model deliberately makes no physical-empty requirement: `_ckpt`, stream, `_snap`, and
`_files` residue may survive catalog deletion and belongs to the perpetual janitor or its
family-specific sweep.

This is only the local half of deletion authority. The production actor must additionally read the
currently adopted parent seal, drain every eligible row, conclusively resolve every exact CAS, and
take a fresh catalog cut before any fold, `REBUILD`, or `DEFER` decision. Those temporal and
cross-object obligations live only in `CaRefPreFoldDrainCore`.

## Remaining lifecycle properties {#carefcatalogcore-lifecycle-properties}

- `INV_NO_ALIAS` proves fresh life ids make residue structurally inert. Reusing a dead id makes the
  reborn life's first read reach predecessor bytes.
- `INV_NEWBORN_SAFE` proves a `Live` row names a life whose `_ckpt` exists. The two controls cover an
  over-broad janitor and a fenced/token-stale creator that installs late.
- `INV_RECONCILE_SAFE` proves reconciliation requires both a terminal creator fence and an exact
  catalog token. A stale unconditional reconciliation can delete a `Live` successor and expose it to
  honest reclamation.
- `INV_BOUNDED_CATALOG` proves no catalog record survives an absent logical name. `_churn` completes
  three create/drop cycles while physical residue remains, while the rejected per-name floor turns
  the bound red.

`INV_RECONCILE_SAFE_BRIEF` remains a finding rather than a gate: the proposed state formula rejects
the legitimate transient state in which a creator's fence becomes terminal before `_ckpt` creation.
The model therefore states the real invariant over observable harm.

## Non-vacuity {#carefcatalogcore-non-vacuity}

The two green configurations exhaust their queues. `_safe` covers two life ids; `_churn` completes
three full lives while debris is allowed to remain. The three negated witnesses establish the paths
that a shortest safety counterexample would otherwise hide:

- `_witness_churn3` reaches the third completed lifecycle with predecessor debris outstanding;
- `_witness_aliasremnant` reaches rebirth over residue left by a completed removal;
- `_witness_orphaneaten` reaches an honest janitor deleting an orphaned running life's data after a
  bad stale reconciliation removed its catalog row.

The catalog model and its runner no longer contain a synchronous removal-cleanup action or a special
physical-cleanup-before-entry-delete gate. Physical reclamation is intentionally asynchronous.

## Reproduce {#carefcatalogcore-reproduce}

From `docs/superpowers/models`:

```bash
./run_refcatalog.sh
```

The runner executes sabotages before green configurations and checks the name of the violated
invariant, not merely TLC's exit status. It uses `-workers 1` so shortest traces and counts remain
reproducible.
