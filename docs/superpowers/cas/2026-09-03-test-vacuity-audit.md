---
description: 'Did the CAS unit tests survive the backend request-contract migration as real tests? A static audit of all 142 test files: counts, the vacuous and thin tests found, the 24 ranked production mutants for the deferred dynamic battery, and the caveats.'
sidebar_label: 'Test-vacuity audit (2026-09-03)'
sidebar_position: 2
slug: /superpowers/cas/test-vacuity-audit-2026-09-03
title: 'CAS test-vacuity audit after the request-contract migration (2026-09-03)'
doc_type: 'reference'
---

# CAS test-vacuity audit after the request-contract migration {#cas-test-vacuity-audit}

**Question (user, 2026-09-03).** The backend request-contract migration (spec
`docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`, revision 13) rewrote most of
the CAS unit-test files. Do they still test something useful, or did some degenerate into `2 + 2 = 4`?

**Method (static half).** Two independent reviewers split the 142 files under `src/Disks/tests/`
(`gtest_cas_*.cpp`, `gtest_ca_*.cpp`) and, for every `TEST`/`TEST_F`/`TEST_P` body, wrote down the
property the name promises, the single production change that must make it red (the "killing
mutation"), and a verdict: DISCRIMINATING (a real killing mutation exists), WEAK (kills only a narrow
or accidental mutation), TAUTOLOGICAL (no production change can make it red), UNSURE. Each reviewer
then ranked twelve production mutants by how many tests each kills. The full per-test tables are the
two sibling files: [auditor A](2026-09-03-test-vacuity-audit/AUDIT-A.md) (73 files) and
[auditor B](2026-09-03-test-vacuity-audit/AUDIT-B.md) (69 files). Base commit of the tables:
`f4190da88a1` for the file split; the tests were read at the post-lock tree (both `CAS*` gates green).

**Caveat.** This is careful manual reasoning against the engine code, not a mutation-testing run. The
dynamic half — apply each ranked mutant, rebuild, run `CAS*`, a mutant nobody kills is a hole — was
deferred by the user; its procedure is recorded in `BACKLOG.md` under
`{#cas-unit-test-mutation-battery}`.

## Counts {#counts}

| auditor | files | tests | DISCRIMINATING | WEAK | TAUTOLOGICAL | UNSURE |
|---|---|---|---|---|---|---|
| A | 73 | 935 | 935 | 0 | 0 | 0 |
| B | 69 | 1393 | 1388 | 3 | 2 | 0 |
| total | 142 | 2328 | 2323 | 3 | 2 | 0 |

## Vacuous and weak tests found {#vacuous-and-weak-tests}

1. `gtest_cas_protocol_scenarios.cpp`, `CASProtocol.DISABLED_RevalidateAbsentTreeDepRecreates` —
   TAUTOLOGICAL: an unconditional `GTEST_SKIP`, no assertion ever runs.
2. `gtest_cas_protocol_scenarios.cpp`, `CASProtocol.DISABLED_AdoptTreeOfReclaimedTreeFailsClosedAtAdoptTime`
   — TAUTOLOGICAL: the same shape.
3. `gtest_cas_record_stream_format.cpp`, `CASRecordStream.WriterIsByteDeterministic` — WEAK: a bare
   reflexive `EXPECT_EQ(f(x), f(x))`; no order shuffle or alternate input.
4. `gtest_cas_ref_recovery_cas_walk.cpp`,
   `CASRefRecoveryCasWalk.PutHookBackendComposesHidingListBackendCasPutFaultInjection` — WEAK: the
   property under test is the fixture's class composition, not production code.
5. `gtest_cas_s3_staging.cpp`, `CASS3Staging.GcBlobDiscoveryPrefixExcludesStagingObjects` — WEAK:
   compares the real `blobsPrefix` against a hard-coded staging-prefix literal instead of the real
   `stagingKeyPrefix` accessor, so a staging-side regression passes.

Thin but DISCRIMINATING (auditor A): `CASFormat.CurrentVersionsAreGBuild` (both functions are
one-line returns, so only a literal typo is caught); `CASFoldSeal.EncodingIsByteDeterministic`
(same-process, same-container encode-twice; the ordering discriminator is the sibling
`TextIsByteDeterministic`); the 17 `CASWireCutDeltas.*` tests (byte-length deltas only; the full-string
`CASEncodingPins` test in the same file covers content).

Placement: items 3–5 are folded into the coverage-gate task of the request-contract plan (Task 21);
items 1–2 are pre-existing disabled scenarios and stay as they are until their scenarios are
implemented or deleted.

## Ranked production mutants for the dynamic battery {#ranked-mutants}

Auditor A (GC, engine, ledger, pool):

1. GC: trust LIST membership/pagination for namespace/edge discovery instead of the `_ckpt`-anchored
   exact-key walk — ~40 tests (`gc_frontier_gate`, `gc_arithmetic_intake`, `gc_bounded_walk`,
   `list_liar_end_to_end`, `holey_list_detector`, `CASFsckAuthority.*`).
2. GC: drop one term from `suppress_destructive` (`frontier_complete AND !hold AND !anomaly`) — ~15
   tests in `gc_frontier_gate`.
3. GC: drop one conjunct from the lease-steal decision — the `CASGCLease` suite (16).
4. GC: let condemn and delete land in the same round — `ABlobCondemnedThisRoundIsNeverDeletedThisRound`,
   two-phase drain, `gc_ack_floor`/fsck pipeline tests.
5. Engine: accept a stale same-key etag on `replace`/`remove` — `CASInMemory.*` token tests, emulated
   token disambiguation, `CASMountLease.HolderBodiesMintFreshAttemptIdsAndFenceCopiesIt`, and most
   fixtures transitively.
6. Ledger: `confirmExactRef` answers Yes under a required-Unknown state — all 26 tests in
   `gtest_cas_confirm_exact_ref.cpp`.
7. Pool: reclaim a same-uuid different-epoch mount lease off a wall-clock stamp without an observation
   certificate — `CASMountAwaitExpiry`/`CASMountObservation` (~10).
8. GC: a counter instead of a set for in-degree source edges — `ReFoldOfRemovalIsIdempotent`,
   `FoldManifestEdgesEmitsOnePlusEdgePerBlob`, most of `gc_source_edge`/`blob_indegree`.
9. Engine: drop the per-ref re-resolve before a destructive action — `CASFsck.PhantomDangling*ReresolvedAway`
   (3), `CASGCFrontierGate.ALateEdgeSparesADeletePendingBlobAtTheDeleteSite`.
10. Engine: skip the exact read/type check of the checkpoint base — `CASFsckAuthority.*ChainBroken/Unchecked`
    (~7) and the cleanup-range validator tests.
11. GC: cache a detected hold instead of re-deriving it from the durable witness each round —
    `ACommittedGapIsRedetectedAndSuppressesEveryRound` and most of `gc_hold_grammar` (35).
12. GC: a constant-true liveness in `CatalogLifecycleReconciler`'s drain loop instead of the per-erase
    authority refresh — `ADeposedLeaderErasesNoCatalogRow`, `ALeaderDeposedBetweenTwoErasesStopsAfterTheFirst`,
    two `CASCatalogLifecycleReconciler` tests.

Auditor B (engine, pool, GC, ledger):

1. Engine: reissue blind instead of settling ambiguity by a resolve read — ~15 in `gtest_cas_requests.cpp`,
   6 in `gtest_cas_slot_occupy.cpp`, most of `gtest_cas_ref_wedge_every_attempt.cpp` (29), wedge sections
   of `gtest_cas_ref_writer.cpp` and `gtest_cas_ref_recovery_cas_walk.cpp`.
2. Engine: drop one of the three fence-admission points (pre-attempt / pre-verb / post-write) —
   `AdmissionIsCheckedAtThreePoints` and most `FenceLost` assertions fleet-wide.
3. Engine: a bare GET instead of the bounded conditional CREATE in the every-attempt wedge rule — all of
   `gtest_cas_ref_wedge_every_attempt.cpp` (29).
4. Engine: credential refresh more than once, or retry classification — 8+ S3 credential tests in
   `gtest_cas_requests.cpp`.
5. Pool: the in-band recovery seal not at exactly `{E, T+1}` with the `prev_epoch_seal` link — all 40 of
   `gtest_cas_ref_recovery_cas_walk.cpp`.
6. Pool: self-remount keeps the stale runtime/wedge — `CASRefWriterRemount` (5).
7. Pool: skip the mandatory HEAD in blob-publication dispatch (condemned → fresh tag, never verbatim) —
   `gtest_cas_upload_detached.cpp` (14) and `gtest_cas_upload_fanout.cpp` (17).
8. GC: weaken `manifestDeletionPremise` — all 12 of `gtest_cas_sweep_deletion_premise.cpp`.
9. GC/ledger: skip the delta-consumption marking in `foldDeltasIntoGeneration` —
   `gtest_cas_txn_apply_ledger.cpp` (7) and, transitively, most GC fold tests.
10. GC: the condemn → graduate → ack-floor → delete cascade stops short of the fixpoint —
    `gtest_cas_truncate_reclaim.cpp` (3, including the B140 regression).
11. Ledger: collapse `commitRefChunk`'s three-arm verdict or the body-before-checkpoint ordering —
    `gtest_cas_ref_writer.cpp` AppendLane and `gtest_cas_ref_snapshot_publish_ordering.cpp` (5).
12. Precommit-first ordering (no blob touch before the precommit's durable write) —
    `gtest_ca_wiring.cpp` `CASWiringPrecommitOrder`/`Pending` (5) and the orphan-blob tests.

Note on auditor B's table: a few rows use a "(+Aborts)" or "N pairs" shorthand for a release test
paired with its debug death-test twin without updating the row's own multiplier tag; the counts above
come from the real per-file `TEST` count, not from summing the tags.
