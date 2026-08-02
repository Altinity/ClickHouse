---
description: 'Phase gate for the v9 CAS ref-chain TLA+ effort: every model, every config, expected vs observed, plus a LIST-trust audit of six models the phase did not rewrite.'
sidebar_label: 'v9 ref-chain TLA+ phase gate'
sidebar_position: 2
slug: /superpowers/models/2026-07-28-v9-phase-results
title: 'CAS v9 ref-chain — TLA+ phase results and gate'
doc_type: 'reference'
---

# CAS v9 ref-chain — TLA+ phase results and gate {#v9-phase-gate}

Phase: `2026-07-28-cas-ref-chain-tla-phase.md` (tasks 1–6), gating spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — the fix for BACKLOG
`{#list-as-journal-dataloss-2026-07-25}`. **TLA+ is phase 0** (spec §9): these models must be green
before any C++ lands, and every property must carry a `_sab_*` config proving it can go red.

This file is the phase's single verdict. Per-model narrative, traces and state counts live in the
six per-model RESULTS files; what is recorded here is the original battery, run end to end from a
clean `tmp/tlc-meta-*` state on 2026-07-28, plus the focused catalog-only pre-fold drain amendment
run on 2026-08-01 and a LIST-trust audit of six models the phase did not rewrite (scope stated
exactly in [that section](#list-trust-audit)).

## Gate {#gate}

> **`TLA PHASE: PASS`**

Every green is green and every `_sab_` is red, across **115 configs in 11 asserted batteries** — the
six current phase model families (88 configs) plus the five older models whose runners this task upgraded
to name assertions (27 configs). No config was skipped, none was left without a
verdict, and no expectation is satisfied by a bare exit code: every red is matched against the NAME
of the invariant or property it had to break.

| model | runner | cfgs | green | red | wall | result |
|---|---|---|---|---|---|---|
| `CaRefTableSnapshotLogCore` | `run_refsnaplog.sh` | 15 | 5 | 10 | 130 s | 15/15 |
| `CaRefDeltaIntakeCore` | `run_deltaintake.sh` | 13 | 5 | 8 | 833 s | 13/13 |
| `CaRefCatalogCore` | `run_refcatalog.sh` | 16 | 2 | 14 | 10 s | 16/16 |
| `CaRefNsCleanupStaleLeaderCore` | `run_nscleanup_staleleader.sh` | 4 | 1 | 3 | 4 s | 4/4 |
| `CaRefPreFoldDrainCore` (+ all-row companion) | `run_prefold_drain.sh` | 18 | 2 | 16 | 12 s | 18/18 |
| `CaCasMountCore` | `run_mount.sh` | 22 | 2 | 20 | 583 s | 22/22 |
| `CaBuildRootPrecommit` | `run_buildrootprecommit.sh` (NEW) | 8 | 4 | 4 | 90 s | 8/8 |
| `CaDiskLifecycle` | `run_disklifecycle.sh` (upgraded) | 7 | 1 | 6 | 5 s | 7/7 |
| `CaErasureProof` | `run_erasureproof.sh` (upgraded) | 6 | 2 | 4 | 9 s | 6/6 |
| `CaRefFoldClampRecoveryCore` | `run_foldclamp.sh` (upgraded) | 2 | 1 | 1 | 1 s | 2/2 |
| `CaRefWriterCleanupCore` | `run_refwcleanup.sh` (upgraded) | 4 | 1 | 3 | 2 s | 4/4 |
| **total** | | **115** | **26** | **89** | **~28 min + focused reruns** | **115/115** |

`CaCasMountCore` also carries a 23rd config on disk, `_rev6_observe`, which is `SLOW=1`-gated: it does
not complete in an interactive budget and that is pre-existing (confirmed against the pre-2026-07-24
committed model). It is excluded from the default battery *with a verdict of its own* (`incomplete`),
not silently.

Reds are not all sabotages, and the distinction matters. The 89 break down as:

- **65 sabotage-class** — a load-bearing rule removed, one per config. 62 are named `_sab_*`; the
  other three are `CaBuildRootPrecommit`'s necessity matrix (`_buggy`, `_buildrootonly`, `_lazyleak`),
  which remove a half of the fix rather than a rule, and are the same kind of evidence.
- **20 reachability witnesses** — negated reachability, where the violation IS the evidence that a
  branch is live (`_witness_*` plus `_b2_witness`).
- **4 FINDINGS** — a red that reports a real defect rather than validating a guard:
  `CaRefDeltaIntakeCore_witness_corruptgap` (spec §5's named residual),
  `CaErasureProof_gc_asbuilt` and `_gc_promptliteral` (the as-built GC, whose windows are why
  lifecycle v1 excised natural erasure promotion), and
  `CaRefCatalogCore_finding_briefreconcileinv` (the task brief's proposed invariant, refuted).

Each is labelled where it appears; none is a phase failure. Two expected RED configurations are liveness checks
(`CaBuildRootPrecommit_lazyleak`, `CaDiskLifecycle_sab_nogcselfexit`), which TLC reports without a
property name — see [the `temporal` expectation kind](#fix-runners) for how those are still asserted.

## What the phase gates, spec section by spec section {#coverage}

| spec | model | the configs that carry it |
|---|---|---|
| §2 **INV-1** per-namespace contiguous ids, the every-attempt rule | `CaRefTableSnapshotLogCore` | `_sab_reuseafterambiguous` (`INV_NO_PHANTOM`), `_sab_gaponfail` (`INV_DENSE`) |
| §2 **INV-2** every epoch transition closed in-band by a `slot-occupy` seal | `CaRefTableSnapshotLogCore` | `_sab_noseal`, `_sab_sealclobbersbase` (`INV_RECOVERY`), `_sab_blindput` (`INV_NO_GHOST`), and **the flip**: `_v9_flip_latepred` GREEN — rev.4's `Late Predecessor PUT` counterexample is now a proof |
| §2 **INV-3** the catalog with ref-layer incarnations | `CaRefCatalogCore`, `CaRefPreFoldDrainCore`, `CaRefPreFoldDrainAllRowsCore` | local exact-row deletion proof: `_sab_deletewithoutevidence`, `_sab_deletewithforeignevidence`, `_sab_deleteunderhold`, `_sab_deletewithoutexactobservation` (`INV_REMOVAL_DELETE_PROVED`); cross-object barrier: `_sab_*_bypasses_drain`, `_sab_continue_after_unknown`, `_sab_stale_delete_after_successor_hold`, `_sab_rebuild_from_unadopted_seal`; consequential stale-cut omission: `_sab_cut_before_list` (`ListedCurrentLifeIsAdmitted`, with `FreshCutFollowsCompletedHotList` additional); opaque-id deletion exactness: `_sab_predecessor_deletes_successor` (`PredecessorProofCannotDeleteSuccessorRemoving`); inert predecessor debris and successor `Removing` admission: `_sab_absent_listed_defers`, `SuccessorRemovingIsAdmitted` plus `_witness_rebirth_with_retained_debris_adopts`; cut-to-intake composition: `_sab_intake_uses_predrain_cut` and `_sab_intake_uses_stale_token` independently falsify id/state/token provenance (`IntakeConsumesFreshPostDrainCut`), plus `_witness_drained_row_absent_from_intake`; all-row rescan and exactness: `_sab_skiprescan`, `_sab_nonexactdelete` |
| §2 **INV-4** `_ckpt` gating of the recovery base | `CaRefTableSnapshotLogCore` | `_sab_cleanupaboveckpt`, `_sab_staleckptcorruption` |
| §3 **Lifecycles**, removal/recreation under a stale leader | `CaRefNsCleanupStaleLeaderCore` | `_sab_noincarnation`, `_sab_rederive` (`NoLiveDataDeleted`) |
| §4 **Recovery** (arithmetic tail, CAS-walk, seal, install) | `CaRefTableSnapshotLogCore`, `CaCasMountCore` | `_v9_safe`/`_v9_safe_deep`; `_witness_genrefused`, `_witness_sealrejected` |
| §5 **fold**, the destructive-round **frontier proof**, the durable **hold** | `CaRefDeltaIntakeCore` | `_sab_skipquietprobe`, `_sab_cleanupignorescursor`, `_sab_deleteignoresindeg` (`NoAckedLoss`), `_sab_destroyunderhold`/`_sab_rebuilddropshold`/`_sab_clearholdonabsent` (`HoldSuppresses`), `_ctl_holdsuppresses` GREEN as their control |
| §5 the **named residual** (corruption above an unwitnessed gap) | `CaRefDeltaIntakeCore` | `_witness_corruptgap` — committed RED, *evidence not failure* |
| §5 **LIST is a zero-trust hint** — the headline flip | `CaRefDeltaIntakeCore` + `CaRefTableSnapshotLogCore` | **the regression pair**: `_v9_hintomission` GREEN (the hint omits everything, forever) beside `_sab_scanistruth` RED (`INV_RECOVERY`) |
| §7 REBUILD preserves holds | `CaRefDeltaIntakeCore` | `_sab_rebuilddropshold`, `_v9_hold` |
| §7 REBUILD drains before its fresh catalog cut | `CaRefPreFoldDrainCore` | `_sab_rebuild_bypasses_drain`, `_sab_rebuild_from_unadopted_seal` |
| §9 r9-5 recovery generations (old-generation install / wedge retry / slot byte-compare) | `CaCasMountCore` | `_sab_staleinstall`, `_sab_wedgeretryoldgen` (`GlobalSupersededWriterMakesNoMutation`), `_sab_slotnocompare` (`AckedOpsAreDurable`) + their three `_strictorder` twins, gated GREEN by `_v9_recoverygen` |

**Deliberate scope note — the temporal lemma's writer-side arms are NOT modelled here.** Spec §5's
normative temporal lemma has three arms. The third (the delete-site in-degree re-read) is modelled
and RED-gated (`_sab_deleteignoresindeg`). The other two — SOURCE-BACKED/TOKENED adoption reading
`Condemned` meta and rematerializing from source, and TOKENLESS relink (`adoptEvidence`) being safe
by ORDERING — are properties of *existing shipped code*, not of the v9 design, and are verified by
the main implementation plan's C++ tests. They are recorded here so their absence from the models is
a stated choice, not a gap nobody noticed.

## Harness output {#harness}

The original battery was executed **sequentially** on 2026-07-28 from a cleared
`tmp/tlc-meta-*` state (215 stale metadirs removed first), each into `tmp/task6_<runner>.log`, with
`timeout 3600` per runner. The amended `CaRefCatalogCore`, `CaRefNsCleanupStaleLeaderCore`, and new
`CaRefPreFoldDrainCore` batteries were run separately on 2026-08-01. The pre-fold battery was rerun
in its review fix round on 2026-08-01. `SECONDS` is wall time per
config on a 32-core host — wall time, not CPU time, and the two runners that use `-workers auto`
(`run_refsnaplog.sh`, `run_deltaintake.sh`) are therefore not comparable with the `-workers 1` ones.

### Every config, expected vs observed {#master-table}

The 2026-07-28 rows are the original runners' output lines. The amended-model rows below are
transcribed from `build/test_task5_prefold_refcatalog_round1_20260801.log`,
`build/test_task5_prefold_nscleanup_round1_20260801.log`, and the generated/state/depth table in
[`CaRefPreFoldDrainCore_RESULTS.md`](CaRefPreFoldDrainCore_RESULTS.md), backed by the clean
`build/task5_review1_prefold_full_gate.log` run. The latter table is the
authority for focused-model counts; this aggregate does not invent a per-config wall time that its
runner did not record.

| model | config | expected | observed | s | verdict |
|---|---|---|---|---|---|
| `CaRefTableSnapshotLogCore` | `_sab_reuseafterambiguous` | violation | violation:INV_NO_PHANTOM | 1 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_gaponfail` | violation | violation:INV_DENSE | 0 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_noseal` | violation | violation:INV_RECOVERY | 1 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_blindput` | violation | violation:INV_NO_GHOST | 0 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_scanistruth` | violation | violation:INV_RECOVERY | 1 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_cleanupaboveckpt` | violation | violation:INV_RECOVERY | 1 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_staleckptcorruption` | violation | violation:INV_NOFAIL | 1 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_sealclobbersbase` | violation | violation:INV_RECOVERY | 0 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_sealclobbersbase_nofail` | violation | violation:INV_NOFAIL | 2 | PASS |
| `CaRefTableSnapshotLogCore` | `_witness_hintlie` | violation | violation:W_NO_HINT_HOLE | 1 | PASS |
| `CaRefTableSnapshotLogCore` | `_sab_noseal_nolate` | green | green | 31 | PASS |
| `CaRefTableSnapshotLogCore` | `_v9_safe` | green | green | 4 | PASS |
| `CaRefTableSnapshotLogCore` | `_v9_flip_latepred` | green | green | 3 | PASS |
| `CaRefTableSnapshotLogCore` | `_v9_safe_deep` | green | green | 41 | PASS |
| `CaRefTableSnapshotLogCore` | `_v9_flip_latepred_deep` | green | green | 43 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_skipquietprobe` | violation | violation:NoAckedLoss | 2 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_cleanupignorescursor` | violation | violation:NoAckedLoss | 17 | PASS |
| `CaRefDeltaIntakeCore` | `_fix_ckptwitness` | green | green | 201 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_adoptbeforecommit` | violation | violation:NoMissedFold | 1 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_destroyunderhold` | violation | violation:HoldSuppresses | 8 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_rebuilddropshold` | violation | violation:HoldSuppresses | 12 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_clearholdonabsent` | violation | violation:HoldSuppresses | 3 | PASS |
| `CaRefDeltaIntakeCore` | `_witness_corruptgap` | violation | violation:NoAckedLoss | 13 | PASS |
| `CaRefDeltaIntakeCore` | `_ctl_holdsuppresses` | green | green | 345 | PASS |
| `CaRefDeltaIntakeCore` | `_sab_deleteignoresindeg` | violation | violation:NoAckedLoss | 11 | PASS |
| `CaRefDeltaIntakeCore` | `_v9_safe` | green | green | 83 | PASS |
| `CaRefDeltaIntakeCore` | `_v9_hintomission` | green | green | 12 | PASS |
| `CaRefDeltaIntakeCore` | `_v9_hold` | green | green | 125 | PASS |
| `CaRefCatalogCore` | `_sab_janitoreatsnewborn` | violation | violation:INV_NEWBORN_SAFE | 0 | PASS |
| `CaRefCatalogCore` | `_sab_zombiegolive` | violation | violation:INV_NEWBORN_SAFE | 1 | PASS |
| `CaRefCatalogCore` | `_sab_reconcilelivecreator` | violation | violation:INV_RECONCILE_SAFE | 1 | PASS |
| `CaRefCatalogCore` | `_sab_reconcilestaletoken` | violation | violation:INV_RECONCILE_SAFE | 0 | PASS |
| `CaRefCatalogCore` | `_sab_deletewithoutevidence` | violation | violation:INV_REMOVAL_DELETE_PROVED | 1 | PASS |
| `CaRefCatalogCore` | `_sab_deletewithforeignevidence` | violation | violation:INV_REMOVAL_DELETE_PROVED | 1 | PASS |
| `CaRefCatalogCore` | `_sab_deleteunderhold` | violation | violation:INV_REMOVAL_DELETE_PROVED | 0 | PASS |
| `CaRefCatalogCore` | `_sab_deletewithoutexactobservation` | violation | violation:INV_REMOVAL_DELETE_PROVED | 1 | PASS |
| `CaRefCatalogCore` | `_sab_sameincarnationrebirth` | violation | violation:INV_NO_ALIAS | 0 | PASS |
| `CaRefCatalogCore` | `_sab_floorretainsdeadname` | violation | violation:INV_BOUNDED_CATALOG | 1 | PASS |
| `CaRefCatalogCore` | `_finding_briefreconcileinv` | violation | violation:INV_RECONCILE_SAFE_BRIEF | 0 | PASS |
| `CaRefCatalogCore` | `_safe` | green | green | 1 | PASS |
| `CaRefCatalogCore` | `_churn` | green | green | 1 | PASS |
| `CaRefCatalogCore` | `_witness_churn3` | violation | violation:WITNESS_CHURN | 0 | PASS |
| `CaRefCatalogCore` | `_witness_aliasremnant` | violation | violation:WITNESS_ALIAS_REMNANT | 1 | PASS |
| `CaRefCatalogCore` | `_witness_orphaneaten` | violation | violation:WITNESS_ORPHAN_EATEN | 1 | PASS |
| `CaRefNsCleanupStaleLeaderCore` | `_sab_noincarnation` | violation | violation:NoLiveDataDeleted | 0 | PASS |
| `CaRefNsCleanupStaleLeaderCore` | `_sab_rederive` | violation | violation:NoLiveDataDeleted | 1 | PASS |
| `CaRefNsCleanupStaleLeaderCore` | `_safe` | green | green | 0 | PASS |
| `CaRefNsCleanupStaleLeaderCore` | `_witness_captureatdeposition` | violation | violation:WITNESS_CAPTURED_BEFORE_REBIRTH | 0 | PASS |
| `CaRefPreFoldDrainCore` | `_sab_fold_bypasses_drain` | violation | violation:DrainBeforeDecision | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_rebuild_bypasses_drain` | violation | violation:DrainBeforeDecision | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_defer_bypasses_drain` | violation | violation:DrainBeforeDecision | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_continue_after_unknown` | violation | violation:DrainBeforeDecision | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_stale_delete_after_successor_hold` | violation | violation:DeleteUsesCurrentAdoptedProof | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_rebuild_from_unadopted_seal` | violation | violation:DeleteUsesCurrentAdoptedProof | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_intake_uses_predrain_cut` | violation | violation:IntakeConsumesFreshPostDrainCut | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_intake_uses_stale_token` | violation | violation:IntakeConsumesFreshPostDrainCut | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_cut_before_list` | violation | violation:ListedCurrentLifeIsAdmitted | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_absent_listed_defers` | violation | violation:DeadListedPredecessorIsInert | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_sab_predecessor_deletes_successor` | violation | violation:PredecessorProofCannotDeleteSuccessorRemoving | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_safe` | green | green | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_witness_takeover_converges` | violation | violation:WITNESS_TAKEOVER_CONVERGES | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_witness_drained_row_absent_from_intake` | violation | violation:WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE | see per-model results | PASS |
| `CaRefPreFoldDrainCore` | `_witness_rebirth_with_retained_debris_adopts` | violation | violation:WITNESS_REBIRTH_WITH_RETAINED_DEBRIS_ADOPTS | see per-model results | PASS |
| `CaRefPreFoldDrainAllRowsCore` | `_sab_skiprescan` | violation | violation:AllEligibleRowsResolvedBeforeDecision | see per-model results | PASS |
| `CaRefPreFoldDrainAllRowsCore` | `_sab_nonexactdelete` | violation | violation:ExactCatalogCAS | see per-model results | PASS |
| `CaRefPreFoldDrainAllRowsCore` | `_safe` | green | green | see per-model results | PASS |
| `CaCasMountCore` | `_sab_epochreset` | violation | violation:WriterEpochMonotoneUnique | 1 | PASS |
| `CaCasMountCore` | `_sab_foreigntakeover` | violation | violation:ForeignUuidNeverAutoTakesOver | 0 | PASS |
| `CaCasMountCore` | `_sab_adoptwedge` | violation | violation:NoPermanentWedge | 1 | PASS |
| `CaCasMountCore` | `_sab_fenceresurrect` | violation | violation:FenceCostsEpoch | 1 | PASS |
| `CaCasMountCore` | `_sab_wallclockreclaim` | violation | violation:GlobalSupersededWriterMakesNoMutation | 1 | PASS |
| `CaCasMountCore` | `_sab_epochwipelive` | violation | violation:SupersededWriterMakesNoMutation | 0 | PASS |
| `CaCasMountCore` | `_sab_decomblindbypass` | violation | violation:FenceCostsEpoch | 7 | PASS |
| `CaCasMountCore` | `_sab_staleinstall` | violation | violation:GlobalSupersededWriterMakesNoMutation | 1 | PASS |
| `CaCasMountCore` | `_sab_wedgeretryoldgen` | violation | violation:GlobalSupersededWriterMakesNoMutation | 1 | PASS |
| `CaCasMountCore` | `_sab_slotnocompare` | violation | violation:AckedOpsAreDurable | 1 | PASS |
| `CaCasMountCore` | `_sab_staleinstall_strictorder` | violation | violation:GlobalSupersededWriterMakesNoMutation | 2 | PASS |
| `CaCasMountCore` | `_sab_wedgeretryoldgen_strictorder` | violation | violation:GlobalSupersededWriterMakesNoMutation | 1 | PASS |
| `CaCasMountCore` | `_sab_slotnocompare_strictorder` | violation | violation:AckedOpsAreDurable | 1 | PASS |
| `CaCasMountCore` | `_stage1` | green | green | 138 | PASS |
| `CaCasMountCore` | `_v9_recoverygen` | green | green | 251 | PASS |
| `CaCasMountCore` | `_witness_reclaim` | violation | violation:W_SameUuidReclaimsExpired | 1 | PASS |
| `CaCasMountCore` | `_witness_remountafterfence` | violation | violation:W_RemountAfterFence | 1 | PASS |
| `CaCasMountCore` | `_witness_observedreclaim` | violation | violation:W_ObservedReclaim | 1 | PASS |
| `CaCasMountCore` | `_witness_recoveryafterobservedreclaim` | violation | violation:W_RecoveryAfterObservedReclaim | 167 | PASS |
| `CaCasMountCore` | `_witness_genrefused` | violation | violation:W_GenerationRefused | 1 | PASS |
| `CaCasMountCore` | `_witness_sealrejected` | violation | violation:W_SealRejectedRetry | 4 | PASS |
| `CaCasMountCore` | `_witness_ackhappened` | violation | violation:W_AckHappened | 1 | PASS |
| `CaBuildRootPrecommit` | `_buggy` | violation | violation:INV_NO_DANGLE_COMMITTED | 0 | PASS |
| `CaBuildRootPrecommit` | `_buildrootonly` | violation | violation:INV_NO_DANGLE_COMMITTED | 1 | PASS |
| `CaBuildRootPrecommit` | `_lazyleak` | temporal | temporal:INV_NO_LEAK | 4 | PASS |
| `CaBuildRootPrecommit` | `_failclosedonly` | green | green | 0 | PASS |
| `CaBuildRootPrecommit` | `_fixed` | green | green | 2 | PASS |
| `CaBuildRootPrecommit` | `_inlineclosure` | green | green | 7 | PASS |
| `CaBuildRootPrecommit` | `_inlineclosure_b2` | green | green | 74 | PASS |
| `CaBuildRootPrecommit` | `_b2_witness` | violation | violation:W_SharedSparedUniqueReclaimed | 1 | PASS |
| `CaDiskLifecycle` | `_sab_nogcselfexit` | temporal | temporal:GcExitsAfterVanished | 1 | PASS |
| `CaDiskLifecycle` | `_sab_notrip2` | violation | violation:I1ForgetTerminal | 1 | PASS |
| `CaDiskLifecycle` | `_sab_unearnedfarewell` | violation | violation:I2EarnedFarewell | 0 | PASS |
| `CaDiskLifecycle` | `_main` | green | green | 1 | PASS |
| `CaDiskLifecycle` | `_witness_forgetdone` | violation | violation:WForgetNeverDone | 0 | PASS |
| `CaDiskLifecycle` | `_witness_joinwindowreclaim` | violation | violation:WNoJoinWindowReclaim | 1 | PASS |
| `CaDiskLifecycle` | `_witness_racedreplaced` | violation | violation:WNoRacedReplaced | 1 | PASS |
| `CaErasureProof` | `_sab_nograce` | violation | violation:TruthEmpty | 1 | PASS |
| `CaErasureProof` | `_gc_promptliteral` | violation | violation:TruthEmpty | 1 | PASS |
| `CaErasureProof` | `_gc_asbuilt` | violation | violation:TruthEmpty | 0 | PASS |
| `CaErasureProof` | `_nogc_grace` | green | green | 1 | PASS |
| `CaErasureProof` | `_fix_gclivegate` | green | green | 5 | PASS |
| `CaErasureProof` | `_witness_promote` | violation | violation:NeverPromoted | 1 | PASS |
| `CaRefFoldClampRecoveryCore` | `_sab_edgegranularity` | violation | violation:NoDeleteBehindClamp | 0 | PASS |
| `CaRefFoldClampRecoveryCore` | `_safe` | green | green | 1 | PASS |
| `CaRefWriterCleanupCore` | `_sab_retirebeforeremoval` | violation | violation:INV_RETIRE_AFTER_REMOVAL | 0 | PASS |
| `CaRefWriterCleanupCore` | `_sab_successorcurrentepoch` | violation | violation:INV_NO_WRONGFUL_RECLAIM | 1 | PASS |
| `CaRefWriterCleanupCore` | `_sab_cancelbeforedurable` | violation | violation:INV_NAMESPACE_REMOVAL_COMPLETE | 1 | PASS |
| `CaRefWriterCleanupCore` | `_safe` | green | green | 0 | PASS |

### Runner verdicts and wall times {#runner-verdicts}

**This block is a normalized transcript, not a paste.** Original rows join the chain driver's
`=== DONE … rc=… seconds=…` line (`tmp/task6_phase_chain.log`, `tmp/task6_old_chain.log`) and the
runner's own final tally (`tmp/task6_<runner>.log`). The two amended rows are labelled explicitly;
their focused logs and per-model RESULTS file are the authority.

Provenance, per row, because "which version of the script produced this" is the whole point of
recording it: the original five **phase** runners ran once in one chain. The catalog runner shown
below is the amended 2026-08-01 re-run, and the pre-fold drain runner is new. The five **older**
runners each report a re-run rather than the chain's first pass — all five after the classifier fix
described below, and `run_foldclamp.sh` / `run_refwcleanup.sh` once more after the review's Minor-6/7
changes (which are classification-only: both came back with the same colours, names and totals).

```
=== DONE  run_refsnaplog.sh             rc=0 seconds=130   ALL EXPECTATIONS MET   (15/15)
=== DONE  run_deltaintake.sh            rc=0 seconds=833   ALL EXPECTATIONS MET   (13/13)
=== DONE  run_refcatalog.sh             rc=0 seconds=10    ALL EXPECTATIONS MET   (16/16)  <- 2026-08-01 focused re-run
=== DONE  run_nscleanup_staleleader.sh  rc=0 seconds=4     ALL EXPECTATIONS MET   (4/4)
=== DONE  run_prefold_drain.sh          rc=0 seconds=12    ALL EXPECTATIONS MET   (18/18) <- 2026-08-01 review fix
=== DONE  run_mount.sh                  rc=0 seconds=583   ALL EXPECTATIONS MET   (22/22)
--- the original phase runners ran in one chain; the two annotated rows are later focused runs ---
=== DONE  run_buildrootprecommit.sh     rc=0 seconds=90    ALL EXPECTATIONS MET   (8/8)
=== DONE  run_disklifecycle.sh          rc=0 seconds=5     ALL EXPECTATIONS MET   (7/7)   <- RE-RUN (rc=1 first pass)
=== DONE  run_erasureproof.sh           rc=0 seconds=9     ALL EXPECTATIONS MET   (6/6)
=== DONE  run_foldclamp.sh              rc=0 seconds=1     ALL EXPECTATIONS MET   (2/2)   <- + Minor-6 re-run
=== DONE  run_refwcleanup.sh            rc=0 seconds=2     ALL EXPECTATIONS MET   (4/4)   <- + Minor-7 re-run
```

**What `<- RE-RUN` means, stated here rather than only in adjacent prose.** In the chain,
`run_disklifecycle.sh` exited **rc=1** — `tmp/task6_old_chain.log` still records
`=== DONE run_disklifecycle.sh rc=1 seconds=5`, and that line is correct. Two of its configs came back
`error` because of a classifier bug in the runner, not a defect in the model; see
[the digit-in-name note](#regex-digits). The bug was fixed and **all five** older runners were then
re-run (the other four were unaffected but were re-run anyway, so that every row here comes from the
scripts as committed). The row above is that re-run.

Two lines that appeared in an earlier draft of this block have been dropped rather than explained:
`PHASE_CHAIN_EXIT=0` and `OLD_CHAIN_EXIT=0` were unconditional `echo`s at the end of the driver
scripts, not exit statuses — the second one was printed by a chain that contained the rc=1 runner
above, so quoting either as evidence would have been misleading.

## Audit: LIST-trust in six models the phase did not rewrite {#list-trust-audit}

The v9 defect is *trusting an enumeration*: §1 — "GC folds what a listing returned and seals a cursor
above what it OBSERVED; a hidden `-1` is a permanent leak, a hidden `+1` deletes acked data", root
cause "absence is undecidable in a sparse id space". Any model that silently assumes a listing is
complete could therefore be resting on the premise v9 exists to remove.

**Scope, stated precisely: this is not a sweep of the tree.** The directory holds 23 modules; the
phase rewrote or extended five, so eighteen were not rewritten, and Task 6's brief named **six** of
those eighteen to audit. Those six are what this section covers. Result: **one encodes no listing at
all, one has an emptiness-`LIST` as its very subject, three assume a complete fold cut, and one
models the incompleteness honestly** — and in every case the model's own subject is unaffected by v9,
so none of the six is rewritten. The other twelve are **deferred, not cleared**; the next audit
target among them is named in [the residuals](#unaudited-residual) below, and it is not a small one.

| model | LIST-trust encoded? | where, exactly | does v9 change its premise? |
|---|---|---|---|
| `CaDiskLifecycle` | **No** | no listing, scan or enumeration appears anywhere in the module | No — nothing to change |
| `CaErasureProof` | **Yes**, as the subject | `ObsList` (`:199`) — the observer's authoritative full-prefix empty-`LIST` | **No — out of v9's scope** (§11), and moot for shipped code (see below) |
| `CaGcAckFloorCore` | **Yes**, and already RED-gated | `GFold` (`:195`) consumes `landed`; the honest arm assumes full consumption unless it DECLARES a clamp | **Yes, in the model's favour** — the assumption becomes a mechanism |
| `CaGcAckFloorZombie` | **Yes**, implicitly | `GBegin` (`:113`) latches `snapIndeg` from the whole `refs` set: the cut is assumed complete at its instant | **Yes, in the model's favour** — same mechanism |
| `CaGcCondemnMarkerGate` | **Yes**, implicitly | `GCut` (`:142`): `folded' = (edge = "landed")` — the round's discovery cut sees the edge iff it had landed | **Yes, in the model's favour** — same mechanism |
| `CaRelinkConfirmCore` | **Yes — modelled honestly, and that is its finding** | `MaxHoles` parameterises the incomplete page; `GFold` folds an observation set | **Yes — it IS the defect v9 fixes**; kept as the permanent regression witness (spec §9) |

### `CaDiskLifecycle` — no LIST-trust encoded {#audit-disklifecycle}

Checked structurally: the module contains no `LIST`, `scan`, `enumerat` or `discover` of any kind.
Every transition is driven by a point read (the `_pool_meta` sentinel; the identity gate's foreign
`pool_id`) or by a local protocol step of `SYSTEM CONTENT ADDRESSED FORGET`. This is not an accident:
lifecycle v1 **excised** the prefix-emptiness leg, so `IdentityLost` is entered directly on
authoritative sentinel absence. Nothing for v9 to change.

### `CaErasureProof` — LIST-trust IS the subject, and it is out of v9's scope {#audit-erasureproof}

`ObsList` is enabled only at `prefix = {}`, where `prefix` is simultaneously the ground truth and
what a full-prefix `LIST` returns — so the model does assume an empty verdict is truthful. Two things
keep that from being a hole in this phase:

1. **What the model probes is the other half of that gate.** The sample is deliberately SPLIT into
   the `LIST` (half 1) and the qualification reads (half 2), with writers, the keeper, GC and the
   eraser free to interleave between them. Three of its six configs are RED precisely because a write
   lands in that window — and two of those three are FINDINGS, not sabotages (`_gc_promptliteral`,
   `_gc_asbuilt`: the as-built GC). So the model does not take the `LIST` as sufficient; it takes it
   as necessary and proves what else is needed.
2. **v9 does not speak to it, and the code path is gone.** v9 decides absence *in a sparse id space*
   by arithmetic and point reads; a pool-prefix emptiness verdict is an existence claim about a whole
   prefix, not a completeness claim about a sequence, and §11 explicitly leaves the mount-time `LIST`
   probe (#23) out of scope. Separately, the natural `Vanished(erased)` promotion these configs gate
   was **excised from the code** by lifecycle v1 — so the two FINDING reds describe a path that no
   longer exists and must not be read as live v9 risk.

**Named residual:** if a full-prefix `LIST` could return empty while objects exist, the erasure proof
would be unsound, and no model in this set covers that. It is recorded here as a named exposure
rather than a silent one; it is not this phase's blocker because the promotion it gates is excised.

### The three complete-cut models — the premise stops being an assumption {#audit-complete-cut}

`CaGcAckFloorCore`, `CaGcAckFloorZombie` and `CaGcCondemnMarkerGate` all assume the round's fold cut
sees everything that had landed by the cut instant. In `CaGcAckFloorCore` the assumption is explicit
enough to be sabotaged: `GFold`'s honest arm may hold back a suffix only if it DECLARES the clamp,
and `SabotageSkipChangedShard` — the *undeclared* skip, "the fold lies about coverage" — is a
committed RED. The other two encode the same premise without a knob for it.

Under v9 that premise stops being an assumption and becomes a mechanism: fold work advances by
arithmetic (`cursor + 1`), an impossible shape HOLDS the namespace (a durable classification-4 hold
that survives REBUILD), and destructive work runs only under a per-namespace frontier proof. So the
failure mode `_sab_skipshard` describes becomes structurally unrepresentable rather than merely
forbidden. **None of the three is rewritten**: their subjects (the ack floor, the deposed-leader
stale-snapshot race, the condemn-marker durability gate) are orthogonal to discovery, and re-modelling
v9's chain inside them would duplicate `CaRefDeltaIntakeCore` at three times the state cost.

One distinction worth keeping straight, because it looks like LIST-trust and is not: the *retired
list* in `CaGcAckFloorCore`/`Zombie` (and the `wView` version a writer has installed) is a **published
control object** read as a whole under token-CAS — `gc/state`, not an enumeration. Nothing about v9
touches it.

### `CaRelinkConfirmCore` — the honest one, kept as the regression witness {#audit-relinkconfirm}

This is the only pre-phase model that refuses the assumption: `MaxHoles` parameterises how many
rounds in a behaviour may return an incomplete page, and `_sab_holeylist` (`MaxHoles = 1` — one page,
once, with **every confirm rule intact**) violates `ConfirmedRelinkNeverDangles`. Per spec §9 it
"becomes the fix's permanent regression witness", so it is NOT rewritten and its red is NOT a phase
failure. The fold-side flip lives in the new models instead, as a pair:

- `CaRefTableSnapshotLogCore_sab_scanistruth` — **RED** (`INV_RECOVERY`): treating a scan as truth
  still breaks recovery, so the property is not vacuous;
- `CaRefDeltaIntakeCore_v9_hintomission` — **GREEN**: under v9 the same omission is harmless.

Neither alone is evidence. A green whose sabotage was never seen red is a green for free; a red with
no green beside it is a defect with no fix. The full note, including what the `_main` caveat now
means, is in
[`CaRelinkConfirmCore_RESULTS.md` § What v9 does to this finding](CaRelinkConfirmCore_RESULTS.md#v9-holeylist-status).

### Spot-checks: the audited colours were re-observed, not quoted {#audit-spotchecks}

The audit cites three configs by colour. None of them sits behind an asserting runner (their models
are driven by single-config wrappers, see [below](#fix-runners)), so each was re-run by hand on
2026-07-28 rather than taken from prose:

| config | claimed | observed (`-workers 1`) |
|---|---|---|
| `CaGcAckFloorCore_sab_skipshard` | RED, the undeclared fold skip | **RED** `INV_NO_DANGLE`, 50,795 / 16,218 |
| `CaRelinkConfirmCore_sab_holeylist` | RED, the historical defect witness | **RED** `ConfirmedRelinkNeverDangles`, 101,966 / 29,638 |
| `CaRelinkConfirmCore_main` (its control, `MaxHoles = 0`) | GREEN | **GREEN**, exhaustive (0 left on queue), 72,984 / 22,165 |

The `_main` counts are byte-identical to its RESULTS table; `_sab_holeylist`'s differ (that table was
produced with `-workers auto`) because a sabotage's state count is "explored before the violation was
found" and varies with worker count. The verdict does not.

## Fixes made while auditing {#fixes}

### (A) A live TLA+ precedence bug in `CaBuildRootPrecommit` {#fix-precedence}

Task 2 found and fixed `dupFlag' = dupFlag \/ (...)` in its own module and reported the same shape
live at `CaBuildRootPrecommit.tla:300`. TLA+ parses `x' = a \/ cond` as `(x' = a) \/ cond`, so when
the right disjunct is true TLC takes it and leaves `x'` unassigned — reported as
`Successor state is not completely specified`, a harness **error**, not an invariant violation. Fixed
here by parenthesising, with the reason in a comment beside it.

The malformed branch is reachable only under `FailClosedCommit = FALSE`, i.e. in `_buggy` and
`_buildrootonly`. Both were run against the **pre-fix** module to establish what the bug actually
cost: both still reported their expected `INV_NO_DANGLE_COMMITTED` counterexample, because BFS
reaches the invariant violation before it ever evaluates the bad disjunct. So the bug was **latent,
not active** — but it was one exploration-order change away (a `-workers auto` run, a bound change, a
new action ordered ahead of `Commit`) from turning a required red into a harness error, and the
retired runner conventions would have counted that error as the expected red. Post-fix, each config
reproduces its own pre-fix search exactly — `_buggy` 394 states / 117 distinct, `_buildrootonly`
7,111 / 1,735 — same counterexample, same counts, both sides of the fix.

A whole-directory scan for the pattern (`' =` followed by an unparenthesised ` \/ ` on the same line,
plus the multi-line continuation shape) found **exactly one** live instance — this one. Every other
ghost latch in the model set, in all 23 modules, already wraps its disjunction. That includes all
the phase models, which adopted the convention from the start.

### (B) Runner discipline: name assertions instead of bare exit codes {#fix-runners}

The phase runners assert *which* invariant a sabotage broke (`violation:${want}`). Auditing the
other runners found four that gate an expectation without that assertion — and two of them were worse
than weak, they were **permanently failing**:

| runner | what it did | what it does now |
|---|---|---|
| `run_disklifecycle.sh` | globbed every cfg, `overall=1` on **any** nonzero TLC exit — so its 3 sabotages and 3 witnesses, whose entire purpose is a nonzero exit, made the suite fail on **every** run | 7 named expectations; sabotages first |
| `run_erasureproof.sh` | same shape: always failed, because 4 of its 6 configs are reds by design | 6 named expectations; the 2 FINDING reds labelled as findings |
| `run_foldclamp.sh` | had expectations, but classified `is violated\|Error:` as "violation" — a parse error or an unrelated invariant passed as the sabotage's own red | 2 named expectations |
| `run_refwcleanup.sh` | derived the expectation from the config NAME (`*_sab_*` ⇒ expect any red) and accepted `is violated\|Error: \|is not enabled` — the same flaw Task 2 removed from its own runner | 4 named expectations (each sabotage cfg declares exactly one invariant, which is now asserted) |

`CaBuildRootPrecommit` had **no runner at all** — its colours lived only in prose in
`cas/06-tla-models.md`, which is how the (A) bug survived. It now has `run_buildrootprecommit.sh`
with 8 named expectations.

All five use `-workers 1` for determinism, per-config `-metadir` (so runners cannot collide), and
`timeout 3600` per config. One expectation kind is new relative to the phase runners:

- `temporal <Prop>` — TLC prints no name for a liveness counterexample
  (`Error: Temporal properties were violated.`), so the assertion is: a temporal violation happened,
  **no** invariant violation happened, and the cfg declares **exactly** the one property named
  (derived from the cfg by a small `declared_properties` helper, never hardcoded, so the label cannot
  go stale behind a property nobody updated the script for). That is as strong as TLC's output
  allows, and it still refuses to let an unrelated red pass.

  **Two** configs are *expected* to take this path — `CaBuildRootPrecommit_lazyleak` (the B199-S2
  leak) and `CaDiskLifecycle_sab_nogcselfexit` (the pre-[C1] scheduler). Two more runners carry the
  arm with no expectation reaching it (`run_foldclamp.sh`, `run_refwcleanup.sh`); `run_erasureproof.sh`
  has none, and that is correct rather than an omission — not one of its six cfgs declares a
  `PROPERTY` at all. An unreached arm is not decoration: `run_foldclamp.sh`'s sabotage breaks its
  liveness property as well as its invariant, and the expectation names the invariant only because
  that is what BFS reports first. Without the arm, a liveness-first report would land in `error` —
  fail-closed, but reading as
  a broken harness instead of "the same sabotage, the other property". Exercised with a liveness-only
  probe (the sabotage cfg with its `INVARIANT` lines stripped, in a scratch tree): the classifier
  returns `temporal:EventuallyFolded` rather than `error`.

A timeout is also classified as its own outcome (`incomplete`, as `run_mount.sh` already does for
`rev6_observe`) rather than falling into `error`, so a config that stops finishing cannot quietly
satisfy anything.

### The new runners' own first pass caught a classifier bug {#regex-digits}

`run_disklifecycle.sh` failed on its first end-to-end run: `_sab_notrip2` and `_sab_unearnedfarewell`
came back `error` instead of the expected red, even though their logs plainly said
`Error: Invariant I1ForgetTerminal is violated.` The cause was in the classifier, not the model — the
invariant-name pattern was `[A-Za-z_]+`, which cannot match a name containing a **digit**
(`I1ForgetTerminal`, `I2EarnedFarewell`). Widened to `[A-Za-z0-9_]+` in all five runners this task
owns, and all five were re-run afterwards so the recorded output comes from the scripts as committed.

Worth stating plainly, because it is the whole argument for name assertions: the old
`run_disklifecycle.sh` would have called both of those configs a **PASS** — they exited nonzero, which
was its entire test. The strengthened runner is what made the model's real behaviour visible, and it
failed **closed**: an unrecognised outcome matches no expectation, so the mistake could only cost a
spurious FAIL, never a spurious PASS.

**Historical follow-up, now closed:** the original five phase runners carried the same
`[A-Za-z_]+` pattern. It was inert in that battery — no config in those 66 runs declared an
invariant or property whose name contains a digit — but it still represented future false-negative
risk. The current phase runners use `[A-Za-z0-9_]+`; the amended catalog and stale-janitor batteries
and the new pre-fold battery were run after that correction.

**Historical scope boundary, now being closed.** Nine runners were single-config drivers
(`./run_X.sh <cfg>`) that asserted nothing by construction — `run_ackfloor`,
`run_ackfloor_zombie`, `run_condemnmarker`, `run_ebo`,
`run_gc_partmanifest`, `run_relinkconfirm`, `run_retiredinrun`, `run_tlc`,
`run_foldabort_witness`. They have no expectation table to strengthen; turning each into an asserted
suite means authoring expectations for 123 more configs across nine models (47 in
`CaGcRootLocalPartManifestCore` alone), which is its own task, not a side effect of this one. The
expectations for those configs currently live in the shell loops of
[`cas/06-tla-models.md` § Running the models](../cas/06-tla-models.md#running-models); after this
round's sweep none of them gates a sabotage on a bare nonzero exit code any more, but the surviving
loops assert nothing at all (expectations live in comments only) — that doc is where the work starts.
Three further models have no runner at all (`CaGcLeaseCore`, `CaGcRoundDeferCore`,
`CaGcShardIncarnationCore`) plus `CaB140DangleMerge`, whose four configs use an `m_*.cfg` prefix
instead of the usual `<Model>_*.cfg` one (so a conventional glob misses them). Recorded as the
follow-up, not silently skipped.

#### First whole-suite conversion: condemn-marker gate {#condemn-marker-whole-suite}

`run_condemnmarker.sh` is the first of those nine drivers converted to an asserted whole-suite
runner. It keeps optional config selection for focused development, but selection cannot weaken the
selected row's exact expectation. The sabotage runs first, each config has a private TLC metadir and
log, and an unknown selector fails closed. The official pinned TLC jar produced:

```text
CONFIG       EXPECT      RESULT                           SECONDS  VERDICT
bug          violation   violation:NoDangle               0        PASS
fix          green       green                            1        PASS

ALL EXPECTATIONS MET
```

The TDD red was the old driver's behavior on `CaGcCondemnMarkerGate_bug.cfg`: it merely forwarded
TLC's nonzero exit and had no assertion that the counterexample was specifically `NoDangle`. The
new runner exits zero only for the exact expected violation plus the green fixed model; a different
invariant, TLC error, timeout, or missing config fails the suite.

#### Ack-floor zombie whole-suite conversion {#ack-floor-zombie-whole-suite}

`run_ackfloor_zombie.sh` now owns all four configs, including the empty-blob boundary added after the
original 123-config inventory. Its prior no-argument invocation stopped at the usage check without
starting TLC; the asserted runner instead executes the complete sabotage-first table. With the official
pinned TLC jar and `TLC_WORKERS=auto`, the focused family gate produced:

```text
CONFIG             EXPECT      RESULT                           SECONDS  VERDICT
sab_eagerdelete    violation   violation:INV_NO_DANGLE          3        PASS
stage1             green       green                            148      PASS
empty_blobs        green       green                            0        PASS
witness_delete     violation   violation:W_DeleteHappens        1        PASS

ALL EXPECTATIONS MET
```

The witness row is deliberately an exact expected violation: accepting any nonzero TLC exit would not
prove that the honest delete path is reachable.

#### Edge-before-observe whole-suite conversion {#edge-before-observe-whole-suite}

`run_ebo.sh` now runs its four independent guard sabotages before the reduced positive model. The old
no-argument invocation failed on an unbound `$1` before TLC started. With the official pinned jar and
`TLC_WORKERS=auto`, the asserted family gate produced:

```text
CONFIG                  EXPECT      RESULT                           SECONDS  VERDICT
sab_late_edge           violation   violation:INV_NO_DANGLE          1        PASS
sab_no_adopt_check      violation   violation:INV_NO_DANGLE          0        PASS
sab_no_k3_adopt_check   violation   violation:INV_NO_DANGLE          0        PASS
sab_no_k3_head          violation   violation:INV_NO_DANGLE          1        PASS
reduced                 green       green                            0        PASS

ALL EXPECTATIONS MET
```

Although all four sabotages target `INV_NO_DANGLE`, each remains a separate row because each removes a
different load-bearing order or adoption guard.

#### Retired-in-run whole-suite conversion {#retired-in-run-whole-suite}

`run_retiredinrun.sh` now selects exactly the five `CaRetiredInRun` configs without accidentally
including the similarly prefixed fold-abort family. Its first asserted run also caught an expectation
error: `sab_attempt_reuse` reaches the smaller empty-journal duplicate-attempt trace first, so the exact
reported invariant is `INV_ONE_PASS`, not `INV_COVERAGE`. The config promises a red attempt-reuse
sabotage, not a coverage-first traversal. After correcting that expectation, the official pinned jar
with `TLC_WORKERS=auto` produced:

```text
CONFIG             EXPECT      RESULT                           SECONDS  VERDICT
sab_attempt_reuse  violation   violation:INV_ONE_PASS           1        PASS
sab_inmem_token    violation   violation:INV_NO_RETURN          1        PASS
sab_no_pacing      violation   violation:INV_NO_LOSS            0        PASS
base               green       green                            1        PASS
empty_blobs        green       green                            1        PASS

ALL EXPECTATIONS MET
```

#### Fold-abort witness whole-suite conversion {#fold-abort-witness-whole-suite}

`run_foldabort_witness.sh` now owns all seven add-only freshness configs. The pre-change no-argument
probe was an interface-level red only: the single-config driver printed usage and did not start TLC.
The semantic red evidence comes from the new sabotage rows themselves, each asserted by exact invariant
name. With the official pinned jar and `TLC_WORKERS=auto`, the family gate produced:

```text
CONFIG                      EXPECT      RESULT                           SECONDS  VERDICT
sab_attempt_reuse           violation   violation:INV_ONE_PASS           0        PASS
sab_gc_clear_on_spare       violation   violation:INV_NO_LOSS            10       PASS
sab_inmem_token             violation   violation:INV_NO_RETURN          3        PASS
sab_no_pacing               violation   violation:INV_NO_LOSS            3        PASS
sab_post_adoption_clear     violation   violation:INV_NO_LOSS            9        PASS
base                        green       green                            164      PASS
empty_blobs                 green       green                            1        PASS

ALL EXPECTATIONS MET
```

#### Ack-floor whole-suite conversion {#ack-floor-whole-suite}

`run_ackfloor.sh` now turns all 19 configs into one sabotage-first gate. The pre-change no-argument
probe was only an interface red: the single-config driver stopped at its usage check before TLC. With
the official pinned jar and `TLC_WORKERS=auto`, the asserted family gate produced:

```text
CONFIG                    EXPECT      RESULT                           SECONDS  VERDICT
sab_ackbeforedrain        violation   violation:INV_NO_DANGLE          1        PASS
sab_ackwithoutread        violation   violation:INV_NO_DANGLE          2        PASS
sab_adopttoken            violation   violation:INV_NO_RETURN          0        PASS
sab_clampnosuppress       violation   violation:INV_NO_DANGLE          1        PASS
sab_ignorefloor           violation   violation:INV_NO_DANGLE          1        PASS
sab_openbeforeload        violation   violation:INV_NO_DANGLE          0        PASS
sab_rebuilddropedge       violation   violation:INV_NO_DANGLE          2        PASS
sab_rebuildkeepretired    violation   violation:INV_NO_DANGLE          1        PASS
sab_rebuildlowround       violation   violation:INV_NO_DANGLE          3        PASS
sab_skipshard             violation   violation:INV_NO_DANGLE          1        PASS
sab_sleeperrearm          violation   violation:INV_NO_DANGLE          0        PASS
stage1                    green       green                            112      PASS
empty_blobs               green       green                            1        PASS
witness_clamp             violation   violation:W_ClampHappens         0        PASS
witness_copyforward       violation   violation:W_CopyForwardHappens   0        PASS
witness_delete            violation   violation:W_DeleteHappens        1        PASS
witness_rebuild           violation   violation:W_RebuildHappens       0        PASS
witness_recreate          violation   violation:W_RecreateHappens      1        PASS
witness_spare             violation   violation:W_SpareHappens         0        PASS

ALL EXPECTATIONS MET
```

#### Relink-confirm whole-suite conversion {#relink-confirm-whole-suite}

`run_relinkconfirm.sh` now gates all 13 configs, including the historical holey-LIST finding and the
empty-receiver boundary. The pre-change no-argument probe was only an interface red: it stopped at the
single-config usage check. With the official pinned jar and `TLC_WORKERS=auto`, the asserted family gate
produced:

```text
CONFIG                   EXPECT      RESULT                               SECONDS  VERDICT
sab_holeylist            violation   violation:ConfirmedRelinkNeverDangles 1        PASS
sab_nofence              violation   violation:ConfirmedRelinkNeverDangles 1        PASS
sab_nogate1              violation   violation:ConfirmedRelinkNeverDangles 1        PASS
sab_nopoison             violation   violation:ConfirmedRelinkNeverDangles 0        PASS
sab_publishafterconfirm  violation   violation:PromotedNeverDangles       1        PASS
sab_stalecache           violation   violation:ConfirmedRelinkNeverDangles 1        PASS
main                     green       green                                1        PASS
main2r                   green       green                                10       PASS
empty_receivers          green       green                                0        PASS
witness_confirmno        violation   violation:W_ConfirmNo                0        PASS
witness_confirmunknown   violation   violation:W_ConfirmUnknown           1        PASS
witness_confirmyes       violation   violation:W_ConfirmYesPromoted       0        PASS
witness_delete           violation   violation:W_BlobDeleted              1        PASS

ALL EXPECTATIONS MET
```

#### Incarnation-core whole-suite conversion {#incarnation-core-whole-suite}

`run_tlc.sh` now owns all 27 `CaIncarnationCore` configs and distinguishes five outcome kinds:
proof-green, exact named safety violation, exact named temporal violation, finite random-simulation
probe, and bounded incomplete BFS. The pre-change no-argument probe was only an interface red. The
first classifier smoke then found two real harness mismatches: official TLC names a liveness failure
as `Temporal property NoLeakForever was violated`, and successful simulation prints simulation markers
rather than BFS's `No error has been found`. Both are now matched exactly; simulation is labelled
`PROBE`, never `PASS`.

The default battery, with the official pinned jar, `TLC_WORKERS=auto` and 1,000 requested simulation
batches, produced:

```text
CONFIG                 EXPECT       RESULT                               SECONDS  VERDICT
sab_cascade            violation    violation:INV_NO_LOSS                 174      PASS
sab_cutoverclaim       violation    violation:INV_NO_DANGLE               4        PASS
sab_foldtimeuniverse   violation    violation:INV_NO_DANGLE               1        PASS
sab_noevreobserve      violation    violation:INV_NO_LOSS                 3        PASS
sab_nofence            violation    violation:INV_NO_DANGLE               1        PASS
sab_norecheckfold      violation    violation:INV_NO_DANGLE               2        PASS
sab_noregistry         violation    violation:INV_NO_DANGLE               1        PASS
sab_noreobserve        violation    violation:INV_NO_DANGLE               1        PASS
sab_noretireview       violation    violation:INV_NO_DANGLE               2        PASS
sab_reusedtag          violation    violation:INV_NO_RETURN               0        PASS
sab_unconddelete       violation    violation:INV_NO_DANGLE               2        PASS
stage1                 green        green                                 143      PASS
stage2                 green        green                                 2        PASS
reval_stage2           green        green                                 2        PASS
stage3                 green        green                                 323      PASS
stage4_journaltree     green        green                                 145      PASS
stage4_small           green        green                                 55       PASS
stage5_small           green        green                                 253      PASS
stage6_cross_smoke     green        green                                 3        PASS
stage6_evstale         green        green                                 46       PASS
stage6_registry        green        green                                 15       PASS
empty_hashes           green        green                                 1        PASS
stage2_live            temporal     temporal:NoLeakForever                4        PASS
hunt_sim               simulation   simulation                            4        PROBE

ALL EXPECTATIONS MET
```

The remaining three rows are deliberately behind `SLOW=1`. A 60-second bounded run of each showed
continued BFS progress, no counterexample, and no queue exhaustion; therefore the result is
`KNOWN incomplete`, not a proof:

```text
CONFIG                 EXPECT       RESULT                               SECONDS  VERDICT
hunt_cross             incomplete   incomplete                            60       KNOWN
stage4                 incomplete   incomplete                            60       KNOWN
stage5                 incomplete   incomplete                            61       KNOWN

ALL EXPECTATIONS MET
```

The bounded prefixes generated respectively 6,529,439, 5,514,071 and 5,102,770 states. A future
queue-exhausted green is accepted loudly as `green (tighten expectation)` so improvement cannot look
like a regression, while any named violation remains a failure.

### The unaudited residual worth naming: `CaGcRootLocalPartManifestCore` {#unaudited-residual}

Of the twelve models outside this audit's scope, one is squarely in the defect's blast radius and
should be the next one audited — it is also the largest battery in the tree at 47 configs, and its
README status is CURRENT (partial):

`CaGcRootLocalPartManifestCore` carries a listing-derived guard in exactly the shape v9 demotes. Its
`listedTok` variable is documented as "live root-shard token discovery observes **from LIST**"
(`:79`), and the Phase-2 token-diff skip is gated on it (`:866`): *"A shard is skippable iff LIST
surfaces a token (`TokenObservable`) AND the observed listed token equals the persisted folded
token"*. That is a **skip decision gated on what a listing returned** — the same premise
`CaGcAckFloorCore`'s `_sab_skipshard` sabotages, but here it is on the honest path, and the skip
advances the durable fold cursor to the journal end while emitting no source edges. Whether v9's
arithmetic frontier changes that model's premise (it very likely does, in the same
assumption-becomes-mechanism direction as the three complete-cut models) is a real question this
audit did **not** answer.

Deferred deliberately: the brief named six files, and 47 configs behind a non-asserting driver is a
task, not a footnote. It belongs with follow-up 2 in [the runner note](#fix-runners) — the same model,
the same 47 configs — so whoever picks up either should pick up both.

## Method notes {#method}

- **Sabotages run first, everywhere.** A green is only evidence once the property it rests on has
  been seen red. Every runner in this phase orders its table that way.
- **Witnesses are negated reachability**: a `W_*` violation is the *evidence* that a branch is
  reachable, not a failure. BFS reports the shortest counterexample, so a branch no red travels would
  silently rot.
- **`-workers 1`** in the phase runners that report numbers: parallel BFS visits states in a
  nondeterministic order, so depth, state counts and *which* shortest counterexample TLC prints all
  vary run to run, while the traces narrated in the RESULTS files are specific action sequences.
- **Findings are not sabotages.** **Four** of the 115 battery configs are reds that report a defect
  rather than validate a guard — `CaRefDeltaIntakeCore_witness_corruptgap`,
  `CaErasureProof_gc_asbuilt`, `CaErasureProof_gc_promptliteral` and
  `CaRefCatalogCore_finding_briefreconcileinv` — matching the count in [the gate](#gate).
  A fifth red of the same kind, `CaRelinkConfirmCore_sab_holeylist`, is the
  historical witness and is **not** in any battery, so it is not in that arithmetic. Each is
  labelled where it appears. The current arithmetic is 65 sabotage-class + 20 witnesses + 4
  findings = 89 reds.
