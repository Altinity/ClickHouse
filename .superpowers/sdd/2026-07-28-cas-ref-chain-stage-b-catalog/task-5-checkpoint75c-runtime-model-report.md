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

## Fix round 1 — real lane composition and isolated missing confirmation

Round base: `fef37240a8ca8f7daa4aabec85b3227a994e013b`.

Independent review found that the original runtime layer did not compose with the actual lane
scalars and that missing-confirm sabotage also violated published-identity validity. Both findings
were reproduced before correction.

### Pre-fix evidence

A deterministic six-state extension retained under `tmp/CaRefLaneCoreBlindSpot.tla` drove the exact
review path:

`StartWrite` → `RemoveCatalogLife` → `RebirthCatalogLife` → `FreshCatalogLookup` → `WriteLands`.

`build/test_CaRefLaneCore_r1_blindspot_property_v2.log` was GREEN for
`NoOldHandleRetarget` (6 generated / 6 distinct / empty queue / depth 6), while the paired
`build/test_CaRefLaneCore_r1_blindspot_witness_v2.log` reached the same final state by violating
`W_PreFixReviewTrace`. At completion `slot_runtime = r2`, but `old_action_target = none`; the real
durable scalar changed without any runtime-scoped effect, proving the blind spot.

Strengthening missing-confirm config to check `PublishedRuntimeHasAcceptedIdentity` before its target
reproduced the collateral failure in
`build/test_CaRefLaneCore_r1_missing_collateral_red.log`: missing confirmation invented
`(life = 0, admitted = 1)`, so TLC reported the non-target published-identity invariant first.

### Correction

The real lane remains single-copy but now carries explicit identity:

- `lane_runtime` binds the lane to the captured old handle and admitted fence generation;
- `StartWriteScoped` writes `attempt_runtime = lane_runtime`;
- `BeginResolveScoped` writes `resolver_runtime = attempt_runtime`;
- rejection, unresolved, retry, application, install failure, and recovery wrappers preserve or
  consume those captures with the corresponding real attempt/resolver transition;
- per-runtime cache and durable projections use the existing bounded marker maps, now ranged over
  `0..MaxTxn`; honest `runtime_cache_marker[lane_runtime] = cache_id` and
  `runtime_durable_marker[lane_runtime] = durable_id` are exhaustive safe invariants;
- `WriteLandsScoped`, `InstallCommittedScoped`, `ObserveDurableScoped`, and
  `ApplyResolutionScoped` derive their target from the captured runtime. Only
  `SabotageOldHandleRetarget` substitutes the current slot, recording a sticky real-lane retarget;
- the standalone abstraction now covers only reader/publisher actions. It records the captured old
  handle and changes no cache/durable projection.

The focused old-handle config uses `RebirthOnly`, so its counterexample is exactly
`StartWriteScoped` → removal → rebirth → successor lookup → `WriteLandsScoped`. The sabotage writes
projection `[r1 |-> 0, r2 |-> 1]`, records `lane_effect_target = r2`, and violates only
`NoOldHandleRetarget`. The honest `witness_rebirtholdaction` drives the same real lane trace, retains
`attempt_runtime = r1`, writes `[r1 |-> 1, r2 |-> 0]`, and records no retarget.

`ConfirmMissingName` now requires an armed empty name slot. Its sabotage reattaches already-observed,
live, identity-valid `r1` without creating or rewriting a runtime. The isolated trace is removal →
exact invalidation → missing confirmation; all non-target invariants hold and only
`MissingNameConfirmationAllocatesNothing` is RED.

All four new sabotage configs enumerate the complete non-target safety set before their expected
target invariant, so another same-state violation would change the reported verdict.

### Fix-round final matrix

| Config | Result | Generated | Distinct | Queued | Depth |
|---|---|---:|---:|---:|---:|
| `sab_noarm` | `ReadyCaughtUp` RED | 3 | 2 | 0 | 2 |
| `sab_dropuncertain` | `ReadyCaughtUp` RED | 68 | 41 | 30 | 4 |
| `sab_appendblocked` | `NoAppendWhileBlocked` RED | 66 | 44 | 32 | 4 |
| `sab_incompleterecovery` | `ReadyCaughtUp` RED | 1,862 | 876 | 505 | 7 |
| `sab_skipidentity` | `InstallMatchesAttempt` RED | 2,974 | 1,377 | 780 | 7 |
| `sab_nofence` | `CertifiedViewIsCurrent` RED | 49 | 34 | 25 | 3 |
| `sab_certifyblocked` | `CertifiedViewIsCurrent` RED | 12 | 10 | 7 | 3 |
| `sab_oldhandleretarget` | `NoOldHandleRetarget` RED | 522 | 225 | 135 | 6 |
| `sab_lateinvalidation` | `ExactPredecessorInvalidationPreservesSuccessor` RED | 594 | 308 | 194 | 5 |
| `sab_fencelosspublication` | `PublishedRuntimeHasAcceptedIdentity` RED | 9 | 8 | 6 | 2 |
| `sab_missingconfirmation` | `MissingNameConfirmationAllocatesNothing` RED | 208 | 115 | 76 | 4 |
| `safe` | GREEN | 721,919 | 216,072 | 0 | 25 |
| `witness_commit` | `W_Commit` reached | 60 | 39 | 28 | 4 |
| `witness_unresolved` | `W_Unresolved` reached | 14 | 11 | 8 | 3 |
| `witness_retrycreated` | `W_RetryCreated` reached | 234 | 135 | 91 | 5 |
| `witness_durableadoption` | `W_DurableAdoption` reached | 711 | 369 | 233 | 6 |
| `witness_recovery` | `W_Recovery` reached | 1,862 | 876 | 505 | 7 |
| `witness_staleresult` | `W_StaleResult` reached | 1,825 | 848 | 486 | 7 |
| `witness_closed` | `W_Closed` reached | 703 | 363 | 229 | 6 |
| `witness_faulted` | `W_Faulted` reached | 707 | 366 | 231 | 6 |
| `witness_rebirtholdaction` | real `W_RebirthOldAction` reached | 773 | 389 | 240 | 6 |
| `witness_selfremount` | `W_SelfRemount` reached | 181 | 108 | 73 | 4 |
| `witness_lateinvalidation` | `W_LateInvalidationPreserved` reached | 594 | 308 | 194 | 5 |
| `witness_missingconfirmation` | `W_MissingConfirmation` reached | 208 | 115 | 76 | 4 |

Final commands and retained logs:

```bash
RUN_ID=20260801-checkpoint75c-r1-final TLC_WORKERS=1 \
  docs/superpowers/models/run_reflane.sh \
  > build/test_CaRefLaneCore_checkpoint75c_r1_final.log 2>&1

TLC_WORKERS=1 docs/superpowers/models/run_prefold_drain.sh \
  > build/test_CaRefPreFoldDrain_checkpoint75c_r1_final.log 2>&1
```

The runtime family reported 24/24 exact verdicts. The honest graph exhausted 216,072 distinct states
at depth 25. The unchanged broader pre-fold gate reported 18/18 exact verdicts. Detailed final
runtime logs are under `build/tlc-runs/reflane/20260801-checkpoint75c-r1-final`.

### Fix-round concerns

None. The fix adds bounded capture/projection state and scoped composition; it does not duplicate the
lane state machine, add runtime history, or change C++.
