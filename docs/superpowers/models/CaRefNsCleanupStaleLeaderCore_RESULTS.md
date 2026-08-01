# `CaRefNsCleanupStaleLeaderCore` TLA+ gate results {#carefnscleanupstaleleadercore-results}

Model: `CaRefNsCleanupStaleLeaderCore.tla`. The rewritten model owns one narrow obligation of the
perpetual `cas/ns/` janitor: a LIST page deposits the exact physical life id encoded in a returned
key, and a delayed delete must continue to target that id after same-name rebirth. It does not model
the pre-fold catalog barrier; `CaRefPreFoldDrainCore.tla` owns that ordering.

Runner: `./run_nscleanup_staleleader.sh`, TLC 2.19 with one worker. The table records the 2026-08-01
run from `build/test_task5_prefold_nscleanup_staleleader_witness_20260801.log` and the corresponding individual TLC logs. All
four expectations passed.

## Results {#carefnscleanupstaleleadercore-results-table}

| Configuration | Expected result | Generated / distinct | Depth |
|---|---|---:|---:|
| `_sab_noincarnation` | `NoLiveDataDeleted` violated | 19 / 9 | 6 |
| `_sab_rederive` | `NoLiveDataDeleted` violated | 28 / 12 | 7 |
| `_safe` | green | 33 / 12 | 7 |
| `_witness_captureatdeposition` | `WITNESS_CAPTURED_BEFORE_REBIRTH` violated | 13 / 7 | 5 |

The green run exhausts its queue. Violation runs stop at the shortest counterexample.

## Property and traces {#carefnscleanupstaleleadercore-property}

`NoLiveDataDeleted == ~deletedLiveData`. The sticky ghost is set only when a resumed janitor delete
targets the currently `Live` life's id and an object at that id actually exists.

The two red controls isolate independent requirements:

- `_sab_noincarnation`: `PreFoldDelete` → `ListCandidate` → `PostPageCutAndNominate` → `Recreate`
  with the listed id → `StaleJanitorDelete`. Even an exact physical delete reaches the reborn life
  if life ids are reused.
- `_sab_rederive`: `PreFoldDelete` → `ListCandidate` → `PostPageCutAndNominate` → `Recreate` with a
  fresh id → `WriteObject` → `StaleJanitorDelete`. Re-resolving the logical name at resume time
  redirects the old job onto the successor's bytes.

In `_safe`, rebirth mints a fresh id and the delete continues to use `listedInc`; the janitor can
resume after rebirth and still cannot name the successor's object. `NominationCapturedPhysicalId`
also checks that every nomination came from an actual listed physical id.

`_witness_captureatdeposition` makes the useful honest route non-vacuous: an old physical id is
listed and nominated from a post-page catalog cut before the same logical name is recreated with a
different id.

## Scope {#carefnscleanupstaleleadercore-scope}

The model represents the production sequence relevant to this hazard:

1. the catalog-only pre-fold drain removes the predecessor's locally proven `Removing` row;
2. a later perpetual-janitor LIST page returns a physical `cas/ns/<life_id>/...` key;
3. a post-page catalog cut proves that id is no longer named and nominates it;
4. the logical name is recreated with a fresh id and writes data;
5. a delayed janitor deletion executes against the captured physical id.

The model intentionally does not duplicate:

- the adopted-parent/fresh-cut A/B protocol, which belongs to `CaRefPreFoldDrainCore`;
- the local evidence, hold, and exact-row checks, which belong to `CaRefCatalogCore`;
- any synchronous physical cleanup in namespace removal, because none exists in the current design.

The catalog is consulted at nomination time, not used later to derive a target. Once nominated, exact
physical identity is the safety boundary; a stale logical name is never carried into deletion.

## Reproduce {#carefnscleanupstaleleadercore-reproduce}

From `docs/superpowers/models`:

```bash
./run_nscleanup_staleleader.sh
```

The runner checks both the expected verdict and the exact violated invariant. It uses one TLC worker
so the reported shortest traces and state counts are reproducible.
