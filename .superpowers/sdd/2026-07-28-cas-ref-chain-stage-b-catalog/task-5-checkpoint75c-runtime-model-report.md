# Task 5 checkpoint 7.5c phase 0 report

## Scope and outcome

Base: `5f359fe20d307eb6f6ff8f1e17dff5d3ce3d2b61` on `cas-gc-rebuild`.

The authoritative `CaRefLaneCore` now composes its existing single six-state append lane with a
bounded identity/cache submodel. The lane remains bound to the captured predecessor runtime; the
cache layer contains two concrete runtime ids (`r1`, `r2`), two durable life ids (`1`, `2`), one
non-authoritative name slot, one old handle, immutable `(life, admitted)` identity records, separate
per-runtime cache/durable markers, one exact delayed-invalidation target, and raw/accepted fence
state. This is the smallest bound that reaches both predecessor/successor coexistence and same-life
self-remount without duplicating the lane product.

No C++ was changed.

## RED evidence

Each new defect is controlled by one constant and reaches its intended named invariant without a
deadlock or `TypeOK` failure.

| Config | Defective trace | Intended RED | Generated / distinct / queued |
|---|---|---|---:|
| `sab_oldhandleretarget` | `RemoveCatalogLife` → `RebirthCatalogLife` → `FreshCatalogLookup` → `OldHandleOperation` | `NoOldHandleRetarget` | 646 / 350 / 227 |
| `sab_lateinvalidation` | `RemoveCatalogLife` → `RebirthCatalogLife` → `FreshCatalogLookup` → `LateExactInvalidation` | `ExactPredecessorInvalidationPreservesSuccessor` | 650 / 354 / 231 |
| `sab_fencelosspublication` | `FenceMove` publishes `r2` at raw generation 2 while only generation 1 is accepted | `PublishedRuntimeHasAcceptedIdentity` | 9 / 8 / 6 |
| `sab_missingconfirmation` | `RemoveCatalogLife` → `ConfirmMissingName` allocates `r2` for absent catalog life 0 | `MissingNameConfirmationAllocatesNothing` | 53 / 39 / 29 |

The retained focused logs are:

- `build/test_CaRefLaneCore_red_oldhandleretarget.log`;
- `build/test_CaRefLaneCore_red_lateinvalidation_v2.log`;
- `build/test_CaRefLaneCore_red_fencelosspublication.log`;
- `build/test_CaRefLaneCore_red_missingconfirmation.log`.

## Final verdict matrix and full state counts

The family runner reported 24/24 expected verdicts. A RED/witness run stops at its intended
counterexample, so its queue count is intentionally nonzero. The honest run exhausts the queue.

| Config | Result | Generated | Distinct | Queued | Depth |
|---|---|---:|---:|---:|---:|
| `sab_noarm` | `ReadyCaughtUp` RED | 2 | 2 | 0 | 2 |
| `sab_dropuncertain` | `ReadyCaughtUp` RED | 69 | 42 | 31 | 4 |
| `sab_appendblocked` | `NoAppendWhileBlocked` RED | 67 | 45 | 33 | 4 |
| `sab_incompleterecovery` | `ReadyCaughtUp` RED | 2,279 | 1,005 | 592 | 7 |
| `sab_skipidentity` | `InstallMatchesAttempt` RED | 3,690 | 1,596 | 920 | 7 |
| `sab_nofence` | `CertifiedViewIsCurrent` RED | 49 | 34 | 25 | 3 |
| `sab_certifyblocked` | `CertifiedViewIsCurrent` RED | 15 | 12 | 9 | 3 |
| `sab_oldhandleretarget` | `NoOldHandleRetarget` RED | 687 | 350 | 227 | 5 |
| `sab_lateinvalidation` | `ExactPredecessorInvalidationPreservesSuccessor` RED | 691 | 354 | 231 | 5 |
| `sab_fencelosspublication` | `PublishedRuntimeHasAcceptedIdentity` RED | 9 | 8 | 6 | 2 |
| `sab_missingconfirmation` | `MissingNameConfirmationAllocatesNothing` RED | 60 | 39 | 29 | 3 |
| `safe` | GREEN | 1,017,625 | 278,718 | 0 | 25 |
| `witness_commit` | `W_Commit` reached | 61 | 40 | 29 | 4 |
| `witness_unresolved` | `W_Unresolved` reached | 14 | 11 | 8 | 3 |
| `witness_retrycreated` | `W_RetryCreated` reached | 252 | 143 | 98 | 5 |
| `witness_durableadoption` | `W_DurableAdoption` reached | 817 | 411 | 267 | 6 |
| `witness_recovery` | `W_Recovery` reached | 2,279 | 1,005 | 592 | 7 |
| `witness_staleresult` | `W_StaleResult` reached | 2,292 | 1,014 | 598 | 7 |
| `witness_closed` | `W_Closed` reached | 826 | 417 | 271 | 6 |
| `witness_faulted` | `W_Faulted` reached | 830 | 420 | 273 | 6 |
| `witness_rebirtholdaction` | `W_RebirthOldAction` reached | 687 | 350 | 227 | 5 |
| `witness_selfremount` | `W_SelfRemount` reached | 188 | 115 | 80 | 4 |
| `witness_lateinvalidation` | `W_LateInvalidationPreserved` reached | 691 | 354 | 231 | 5 |
| `witness_missingconfirmation` | `W_MissingConfirmation` reached | 60 | 39 | 29 | 3 |

The unchanged broader pre-fold gate also reported 18/18 expected verdicts: 16 intended REDs and two
GREEN configurations.

## Commands and retained logs

Focused RED commands used the same form, with one config per run and redirected output:

```bash
/usr/bin/java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC \
  -metadir ../../../build/tlc-meta/reflane-red-oldhandleretarget -workers 1 \
  -config CaRefLaneCore_sab_oldhandleretarget.cfg CaRefLaneCore.tla \
  > ../../../build/test_CaRefLaneCore_red_oldhandleretarget.log 2>&1
```

Final family and broader-gate commands:

```bash
RUN_ID=20260801-checkpoint75c-final2 TLC_WORKERS=1 \
  docs/superpowers/models/run_reflane.sh \
  > build/test_CaRefLaneCore_checkpoint75c_final2.log 2>&1

TLC_WORKERS=1 docs/superpowers/models/run_prefold_drain.sh \
  > build/test_CaRefPreFoldDrain_checkpoint75c.log 2>&1
```

Detailed family logs are retained under
`build/tlc-runs/reflane/20260801-checkpoint75c-final2`; metadata is under
`build/tlc-meta/reflane/20260801-checkpoint75c-final2`.

## Vacuity and writer self-review

Every transition capable of writing slot/runtime identity or runtime-local data is enumerated here:

- `Init` publishes `r1`, captures the old handle, and fixes identity `(1, 1)` in both the runtime
  record and immutable ledger.
- `FenceMove` advances raw generation `1 → 2` and honestly clears the slot while leaving the old
  handle/runtime alive. Its sole sabotage branch publishes `r2` at unaccepted generation 2; the
  two-state counterexample proves this path is reachable.
- `AcceptRearm` is enabled only after `BeginRearm`, advances raw generation `2 → 3`, accepts 3, and
  then publishes distinct `r2` with same-life identity `(1, 3)`. `FailRearm` returns to `Fenced` and
  cannot write the slot, live set, or either identity map.
- `FreshCatalogLookup` is enabled only for observed successor life 2 while armed and assigns the
  previously unused `r2` exactly once as `(2, 1)` before publishing it.
- `LateExactInvalidation` is the only honest post-publication slot clearer. It compares the captured
  runtime id and therefore leaves an attached different runtime unchanged. Its sabotage clears a
  reachable attached `r2` while the pending target is `r1`.
- `ConfirmMissingName` honestly leaves the slot, live set, identity record, and ledger unchanged.
  Its sabotage allocates identity `(0, 1)`, which reaches the named allocation invariant.
- `OldHandleOperation` is the only writer of per-runtime cache/durable markers. Honest append, read,
  publisher, and resolver choices all derive their target from `old_handle_runtime`; the retarget
  sabotage alone derives it from `slot_runtime`, and reaches `r2` only after successor publication.

`RemoveCatalogLife` and `RebirthCatalogLife` write catalog observation state but do not rewrite any
runtime identity. `BeginRearm` and `FailRearm` write only arm phase. All original lane transitions
are conjoined with `UNCHANGED runtimeVars`, so lane authority loss can stale predecessor work but
cannot retarget or rewrite its identity. `RuntimeIdentityImmutable` cross-checks every live runtime
against its once-assigned ledger entry; `PublishedRuntimeHasAcceptedIdentity` independently checks
catalog observation, accepted generation, and no-slot-while-fenced.

The four new witness traces demonstrate non-vacuity of all honest sides: two concrete same-name
runtimes coexist and old work writes only `r1`; self-remount reaches `(1, 3)` only after successful
re-arm; delayed invalidation preserves successor `r2`; and missing-name confirmation completes
without changing allocation state.

## Concerns

None. The model deliberately retains at most two bounded runtime records; it does not create an
unbounded retired-history set or forbid same-name reuse.
