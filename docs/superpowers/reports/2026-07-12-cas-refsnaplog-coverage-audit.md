# CAS Ref Snapshot+Log Refactor — Test-Coverage Audit (Task 13a)

Baseline `a39032d51fa` (last commit before the Task-10 writer switch) → HEAD `ea180d83d58`
(branch `cas-gc-rebuild`). Audits the Task 10-13 rewrite of the CAS unit suites: verifies nothing
important stopped being tested, and closes every gap with discriminating tests.

Session: <https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk>

## Scope of the change

`git diff a39032d51fa..HEAD --stat -- src/Disks/tests/`: 28 files, +2946 / −3559.
One file fully deleted (`gtest_cas_gc_token_diff.cpp`), three added (`gtest_cas_ref_gc.cpp`,
`gtest_cas_ref_intake.cpp`, `gtest_cas_ref_writer.cpp`). 67 `TEST()`/`TEST_F()` bodies were deleted or
renamed across the suite; every one is classified below.

## Part 1 — Contract inventory (every deleted/renamed test classified)

Classification: **P** preserved-equivalent (name the carrying test), **O** obsolete (concept removed by
spec; cite section), **GAP**. Zero unclassified.

### RootShard codec round-trips — `gtest_cas_codecs.cpp` (13, all O)

`CasRootShardCodec.{CasHeaderRoundTrips, EmptyManifestRoundTrips, IncarnationRoundTrips,
LargeJournalRoundTrips, OptionalBindingAbsenceIsDistinguished, OwnerKindBuildIdInvariantIsEnforced,
ProtobufEncodingIsDeterministic, RefsCanonicalOrderRegardlessOfInsertion, RoundTripInterleavedOwnerEvents,
CasHeaderFutureCompatibilityVersionThrowsUnknownFormatVersion, FailClosedOnGarbageBytes,
JournalTransitionVersionMonotonicityIsEnforced}` + `CasByteOrderGolden.RootShardBigEndian`.

**O** — pure codec round-trips of the `RootShardManifest` type, which is **deleted** per Global
Constraints "No compatibility scaffolding" and Task 12(h) grep gate (`grep -rn "RootShard" src/` = 0).
Replacement wire formats are covered by `gtest_cas_ref_codecs.cpp` — 78 tests over `CasRefCodec`
(render/parse canonicality + rejects, each op-kind round-trip, every decode/encode validation) and
`CasRefSnapshotCodec` (Live/Removed round-trip, byte-identical re-encode, unsorted/duplicate/
non-canonical/oversized rejects). The deterministic-encoding and fail-closed-on-garbage contracts
survive as `CasRefCodec.OrderMatchesLexicalOrderOfRender` / `DecodeRejectsNonHexGarbage` /
`DecodeRejectsFutureFormatVersion` and their snapshot analogues.

### Token-diff discovery — `gtest_cas_gc_token_diff.cpp` (9, file deleted)

`CasGcDiscovery.{FailsClosedToReadWhenListKeyAmbiguous, FailsClosedToReadWhenNoPriorCoverage,
FailsClosedToReadWhenTokensUnobservable, ReadsShardWhenTokenAdvancedOrMissing, SkipsQuiescedShard,
UniverseIsPresentRefShardsFromList}`, `CasBackendListTokens.{InMemorySupportsListTokens,
OverridableToFalse}`, `CasShardCoverageRoundTrip.FoldedTokenAndCursorSurviveEncodeDecode`.

- The six `CasGcDiscovery` + two `CasBackendListTokens`: **O**. The token-diff "did-it-change" Skip
  machinery has no analogue (spec §gc-round-algorithm; task-12 report decision 5). The "did it change"
  signal is now simply logs above the per-table cursor. `listTokens` has **zero production callers at
  HEAD** (`git grep listTokens HEAD -- …ContentAddressed/ ':!*tests*'` = empty), so the capability and
  its tests are genuinely dead, not silently untested. The fail-closed intent survives as
  `CasRefGc.MalformedRefKeyAbortsRefFoldingNoPartialDelta` (a malformed key aborts the whole round's
  ref folding, no partial delta) and the baseline guard (see GAP-A).
- `CasShardCoverageRoundTrip.FoldedTokenAndCursorSurviveEncodeDecode`: **P** →
  `CasDanglingPrecommit.ShardCoverageRoundTripsLastFoldedRefId` (the coverage struct dropped the mutable
  `folded_token`/`min_live_precommit` and now round-trips the `last_folded_ref_id` cursor).

### GC-side precommit reclaim (moved to the writer)

- `gtest_cas_dangling_precommit.cpp`: `AbandonedPrecommitOrphansManifestUntilFix`,
  `DoubleRemovalOfReclaimedPrecommitIsIdempotent`, `ReclaimIsIdempotentAndSelfTerminating`,
  `SkipPreservedForLivePrecommitAndForNoPrecommit` — **O**; and
  `ShardCoverageRoundTripsMinLivePrecommit` → **P** `ShardCoverageRoundTripsLastFoldedRefId`.
- `gtest_cas_build_root_dangle.cpp`: `CasBuildRoot.AbandonedPrecommitReclaimed` — **O**.
- `gtest_cas_gc_leak.cpp`: `CasGcLeak.AbandonedPrecommitReclaimsOwnBlobs` — **P** (blob-reclaim contract)
  → `CasRefGc.EdgeCancellationAddThenRemoveReclaimsBlob`.

Spec §clean-up-old-precommits + §Failed Precommit Cleanup move dead-precommit removal to the **writer**
(it appends exact `owner_transition` removals; GC no longer reclaims). The writer contract is asserted
by `RefWriterStalePrecommitSweep.SweepsOnlyStaleEpochPrecommitsKeepsCurrentEpoch` and
`BoundedBatchesAndInterruptionResumeAcrossMounts`. The reclaimed blobs are then condemned by ordinary
edge cancellation (`CasRefGc.EdgeCancellationAddThenRemoveReclaimsBlob`).

### Mutable shard incarnation / ABA (no mutable shard object)

- `gtest_cas_gc_shard_incarnation.cpp`: `ActivatedPrecommitBlocksShardReclaim`,
  `DroppedShardObjectIsReclaimed`, `IdleButLiveShardNotReclaimed`, `ReviveRacesReclaimAborts` — **O**;
  `RecreateAfterReclaimFoldsFromZero` — **P** (recreation via a strictly-greater `RefTxnId`, folded by
  `CasRefGc.RemovedNamespaceCoveredLogsCleanedByCompletingRound` + writer
  `RefWriterNamespaceBirth.BirthFromRemovedRejectedWithoutMarkerAcceptedWithMarker`).
- `gtest_cas_gc_fold.cpp`: `IncarnationMismatchRestartsFoldAtZero`,
  `IncarnationMismatchRestartsFoldAtZeroMultiShard` — **O**.

There is no mutable per-namespace shard object in the immutable model, so token-guarded reclaim and the
incarnation-reset-to-zero fold are gone (spec §object-layout, §responsibility-boundary). ABA is
impossible: `RefTxnId` is strictly increasing (`CasRefStateMachine.StrictlyIncreasingTxnIdsRejectsEqualAndLower`),
so a recreated namespace never reuses an id a stale delete could match.

### Journal trim (immutable logs are not trimmed)

- `gtest_cas_gc_round.cpp`: `LazyTrimCompactsAtThresholdOrSoftLimit`,
  `LazyTrimSkipsSmallJournalAndKeepsTokenStable`, `MaintenanceTrimCompactsEverythingOnce` — **O**;
  `TrimOnlyBelowSealedCoverage` — **P** (the only-delete-what-is-covered safety contract) →
  `CasRefGc.RefObjectCleanupHonorsAllThreeConditions` + `CasRefIntake.PlanRefCleanupThreeConditions`.
- `gtest_cas_gc_resume.cpp`: `CasGcRound.TrimDropsFoldedOwnerEvents` — **P** →
  `CasRefGc.RemovedNamespaceCoveredLogsCleanedByCompletingRound`.

Spec §gc-step-clean-ref-objects: in-place journal trim is replaced by ref-object cleanup that deletes a
covered `_log`/superseded `_snap` only when BOTH the durable cursor and an observed snapshot cover it.
The safety half (never delete an object a completing reader needs) is the surviving contract.

### CasStore shard lane, backpressure, decode-TTL, single-flight — `gtest_cas_store.cpp` (21; 2 added)

| Deleted test | Class | Carrying test / spec |
|---|---|---|
| `CasShardQueue.CoBatchesTwoRefsIntoOneCasPut` | P | `RefWriterAppendLane.CompatibleMutationsShareOneCreate` |
| `CasShardQueue.ThrowingClosureIsIsolatedFromBatch` | P | `RefWriterAppendLane.InvalidBatchEntryGetsOwnExceptionBatchSurvives` |
| `CasShardQueue.ConflictReplaysBatchExactlyOnce` | P | `CasRequestController.UncertainResolvesIdenticalAsCommitted` + `RefWriterAppendLane.WedgedAppendObservedDurableAppliesBeforeNextId` |
| `CasStore.DropRefSurvivesCasConflict` | P | same (uncertain-write resolve = no double-append) |
| `CasShardQueue.StressNoConflictsNoLostMutations` | P | single-consumer append lane (`use_count()==1` split-brain gate, CasStore.cpp:1129) + concurrent-drop `RefWriterAppendLane.CompatibleMutationsShareOneCreate` |
| `CasShardQueue.SameRefMutationsSplitAcrossFlushes` | **GAP-B** | `seen_refs` batch cut CasStore.cpp:1399 (`CasRefBatchScopeCuts`) — untested |
| `CasStoreBackpressure.*` (6) | O | root-shard-body-size delay removed; `manifest_max_delay_ms` has zero read sites (vestigial config) |
| `CasStoreDecodeTtl.WarmHitWithinTtlSkipsHead` | P | `RefWriterAppendLane.WarmIsolatedMutationCostsOneCreateZeroReads` (whole-table cache) |
| `CasStoreDecodeTtl.ForceFreshAlwaysHeads` | O | root-shard decode cache gone; `ForceFresh` now lives in the part-folder facade (`gtest_cas_part_folder_access.cpp`) |
| `CasStoreDecodeTtl.ConcurrentWriteDuringGetDoesNotPoisonStaleEntry` | O | root-shard decode-TTL cache removed |
| `CasStore.DropNamespaceTombstonesAndRemovesFiles` | P | `CasStore.DropNamespaceRemovesEveryOwnerButLeavesFilesForGc` (owners) + file deletion moved to GC (`CasRefGc.RemovedNamespaceCoveredLogsCleanedByCompletingRound`) |
| `CasStore.MountpointObjectRoundTrip` | **GAP-C** | put+existsFile only in `gtest_ca_wiring.cpp`; get-returns-bytes + remove unasserted |
| `CasStore.RebornShardIncarnationStrictlyGreater` | O | no shard incarnation; `RefTxnId` monotonicity subsumes |
| `CasStore.ShardBornCarriesIncarnation` | O | no shard-born event |
| `CasStoreSingleFlight.ConcurrentResolvesCoalesceToOneHead` | O | root-shard single-flight resolve removed; part-folder facade coalescing tested separately |
| `CasStore.UpdateRefPayloadMutatesWithoutJournal` | P | `CasStore.UpdateRefPayloadUpdatesMutableFiles` (manifest-ref immutability now **type-enforced** by `RefMutableFilesUpdate` exposing only `mutable_files`) |

Backpressure verification: `manifest_soft_limit`/`manifest_hard_limit`/`manifest_max_delay_ms` are
plumbed disk-config→`PoolConfig` but have **no read site** in any backpressure delay path in
`CasStore.cpp`/`CasGc.cpp` (`applyBackpressure` / delay-on-body-size = zero hits). The mechanism the six
`CasStoreBackpressure` tests guarded is dead code; the config fields are vestigial (minor cleanup, see
Concerns). No live behavior lost its only test.

### Renames / adaptations (P)

- `gtest_cas_build.cpp`: `TwoBuildsPublishToSameShardSerialize` → **P**
  `TwoBuildsPublishToSameNamespaceBothLand`; `PublishOwnThreadConflictRetries` → **P** (own-thread
  shard-CAS retry removed with the shard lane; the uncertain-write-resolve contract is
  `CasRequestController.UncertainResolvesIdenticalAsCommitted` — task-12 report §Final verification).
- `gtest_cas_observability.cpp`: `CaInspectDecodesRootShardToJson` → **P**
  `CaInspectDecodesRefLogToJson` + `CaInspectDecodesRefSnapshotToJson`.
- `gtest_cas_pluggable_hash.cpp`: `ReaderGenerationIsRaisedToTwo` → **P** `ReaderGenerationIsRaisedToThree`
  (pool-format generation bump 2→3 for the ref-snaplog format; the fail-closed floor in `decodePoolMeta`
  is the Task 12(g) gate).
- `gtest_cas_gc_rebuild.cpp`: `CasGcBaselineGuard.FreshStateOverTrimmedJournalsFailsClosed` — the
  **contract is not obsolete**: it survives adapted at `CasGc.cpp:927` (spec §Offline Recovery) but the
  positive fail-closed trip is **untested** at HEAD → **GAP-A** (see Part 4). The two HEAD
  `CasGcBaselineGuard` tests cover only the fresh-pool pass case and a *different* guard (adopted seal
  missing, `CasGc.cpp:784`).

### Contract-inventory summary

67 deleted/renamed tests, all classified, zero unclassified:

- **18 preserved-equivalent** (incl. 5 renames): each names the carrying HEAD test above.
- **46 obsolete**: each cites the spec section / removed mechanism (RootShard codec 13; token-diff
  discovery + `listTokens` 8; GC-side precommit reclaim 5; mutable shard incarnation/ABA 6; journal
  trim 3; CasStore root-shard-lane backpressure/decode-TTL/single-flight/shard-born 11).
- **3 GAP** → closed in Part 4: GAP-A (baseline guard positive trip), GAP-B (same-ref batch cut),
  GAP-C (mountpoint roundtrip). Part 2 adds two more from the coverage enumeration (GAP-D, GAP-E), for
  five gap tests total.

### Tracked micro-item (a) — ref_gc oracle-SKIP comment

The brief's micro-item (a) — reword the `gtest_cas_ref_gc.cpp` "no oracle divergence" comment to state
the oracle takes the SKIP path (positive path = `PublishedSnapshotMatchingReplayIsClean`) — is
**already satisfied at HEAD**. The comment at `gtest_cas_ref_gc.cpp:394-400` already explains the SKIP
path (`snapshot_oracle_checked` stays 0 because a cleaned covered log makes the oracle unavailable, not
an error) and names `CasFsckSnapshotOracle.PublishedSnapshotMatchingReplayIsClean`
(`gtest_cas_fsck.cpp:253`) as the positive byte-compare witness. No change needed.

## Part 3 — Spec cross-check (plan Tasks 4-13 failing-test bullets → live HEAD test)

| Task | Bullet | Live test at HEAD |
|---|---|---|
| 4 | 1 HTTP attempt per conditional write | `CasRequestControl.NativeConditionalPutCountsOneAttemptAndCommitted`, `SingleAttemptRetryStrategyRefusesAndCountsEveryConsultation` |
| 4 | classification table (each row) | `CasRequestControl.{ClassifiesPreconditionFailedAsUnresolved, ClassifiesTimeoutAsUnresolved, ClassifiesConnectionResetAsUnresolved, Classifies5xxAsUnresolved, ClassifiesMalformedRequestAsDefiniteFailure, ClassifiesEntityTooLargeAsDefiniteFailure, ClassifiesAccessDeniedAsDefiniteFailure, UnrecognizedErrorsFailSafeToUnresolved, SuccessIsAlwaysCommitted}` |
| 5 | uncertain→GET-identical=Committed | `CasRequestController.UncertainResolvesIdenticalAsCommitted` |
| 5 | uncertain→GET-different=corruption | `CasRequestController.UncertainResolvesDifferentThrowsCorruption` |
| 5 | uncertain→GET-absent=Unresolved, same key | `CasRequestController.UncertainResolvesAbsentRetriesSameKeyWithinBudget` |
| 5 | budget exhaustion=Unresolved | `CasRequestController.OperationDeadlineExhaustionReturnsUnresolvedBeforeMaxAttempts` |
| 5 | fence-lost before attempt=no attempt | `CasRequestController.FenceLostBeforeAttemptSendsNoAttempt` |
| 5 | fence-lost after write=no Committed | `CasRequestController.FenceLostAfterWriteNeverReturnsCommitted` |
| 5 | invalid config rejected at open | `CasRequestController.ValidateBudgetRejects{AttemptTimeoutPlusMarginAtOrAboveLeaseTtl, AttemptTimeoutAboveOperationDeadline, ZeroMaxAttempts, OverflowingSumRatherThanWrapping}` |
| 6 | hex render/parse canonical + rejects | `CasRefCodec.{RenderCanonicalForm, ParseRoundTrip, ParseRejectsShort, ParseRejectsLong, ParseRejectsUppercase, ParseRejectsZeroComponent, ParseRejectsNonHexGarbage, ParseRejectsMisplacedSeparator}` |
| 6 | tuple order == lexical order (property) | `CasRefCodec.OrderMatchesLexicalOrderOfRender` |
| 6 | round-trip each op kind | `CasRefCodec.RoundTrip{NamespaceBirth, RemoveNamespace, SetPayload, OwnerTransitionAdd, OwnerTransitionRemoval, OwnerTransitionReplace, MultipleOpsInOneTransaction}` |
| 6 | every validation rejection | `CasRefCodec.{EncodeRejectsZeroTxnId, DecodeRejectsFutureFormatVersion, DecodeRejectsTruncatedBuffer, DecodeRejectsUnknownOpKind, DecodeRejectsBodyNamespaceMismatch, DecodeRejectsBodyTxnIdMismatch, Encode/DecodeRejects*RefName/ManifestRef/Ops/Bytes}` (28 rejection cases) |
| 7 | round-trip Live and Removed | `CasRefSnapshotCodec.{RoundTripLive, RoundTripLiveEmpty, RoundTripRemoved}` |
| 7 | byte-identical re-encode | `CasRefSnapshotCodec.ByteIdenticalReencode` |
| 7 | rejects unsorted/dup/non-canon/oversized | `CasRefSnapshotCodec.{EncodeRejectsUnsortedCommitted, EncodeRejectsDuplicateCommittedRefName, EncodeRejectsUnsortedPrecommits, EncodeRejectsNonCanonical*, EncodeRejectsOversizedSnapshot}` |
| 8 | key round-trips 3 kinds | `CasLayout.RefObjectKeyRoundTrips` |
| 8 | `_cleanup` < `_log` < `_snap` order | `CasLayout.RefObjectKeyLexicalOrder` |
| 8 | manifest hex path round-trip | `CasLayout.ManifestKeyHexRoundTrip` |
| 8 | non-canonical parse rejections | `CasLayout.ParseRefObjectKeyRejections` |
| 9 | every transition precondition | `CasRefStateMachine.*` (48 tests: birth/owner/promote/payload/removal preconditions, promote atomicity, strictly-increasing ids) |
| 9 | replay equation (property) | `CasRefStateMachine.ReplayEquationPropertyTest` |
| 9 | `admits` budget rejections | `CasRefStateMachine.AdmitsRejectsGrowthPast{SnapshotBudgetOwnerTransitionAdd, SnapshotBudgetSetPayload, SnapshotBudgetPromoteWithPayload, RemovalBudget}` + `AdmitsExactnessPropertyTest` |
| 10 | empty+birth recovery | `RefWriterRecovery.{EmptyNamespaceRecoversToEmptyState, BirthOnlyLogNoSnapshotRecoversToEmptyLiveTable}` |
| 10 | snapshot+tail recovery | `RefWriterRecovery.SnapshotPlusTailRecovery` |
| 10 | recovery restart on vanish, converges | `RefWriterRecovery.RestartOnVanishConvergesOnNewerSnapshot` |
| 10 | wedged lane blocks same-table, other proceeds | `RefWriterAppendLane.WedgedLaneBlocksSameTableWhileOtherTableProceeds` |
| 10 | wedged observed-durable applies before next id | `RefWriterAppendLane.WedgedAppendObservedDurableAppliesBeforeNextId` |
| 10 | failed queue entry own exception, batch survives | `RefWriterAppendLane.InvalidBatchEntryGetsOwnExceptionBatchSurvives` |
| 10 | warm mutation = 1 create, 0 reads | `RefWriterAppendLane.WarmIsolatedMutationCostsOneCreateZeroReads` |
| 10 | B compatible mutations share 1 create | `RefWriterAppendLane.CompatibleMutationsShareOneCreate` |
| 10 | (scope 10d) one op per ref name per batch | **GAP-B** — `seen_refs` cut untested |
| 11 | threshold + mount-time snapshot triggers | `RefWriterSnapshotPublish.{ThresholdTriggerPublishesCacheReplayEquivalentBytes, MountTimeTriggerPublishesAfterRecoveryReplaysLargeTail}` |
| 11 | grace age respected | `RefWriterSnapshotPublish.GraceAgeRespectedYoungLogNotCovered` |
| 11 | snapshot never blocks concurrent append | `RefWriterSnapshotPublish.PublicationNeverBlocksConcurrentAppend` |
| 11 | cache-replay == published bytes | `RefWriterSnapshotPublish.ThresholdTriggerPublishesCacheReplayEquivalentBytes` |
| 11 | successor cleanup bounded batches, resume | `RefWriterStalePrecommitSweep.{SweepsOnlyStaleEpochPrecommitsKeepsCurrentEpoch, BoundedBatchesAndInterruptionResumeAcrossMounts}` |
| 11 | removal txn names owners then remove_namespace | `RefWriterNamespaceRemoval.TxnNamesEveryOwnerThenRemoveNamespace` |
| 11 | repeated drop success, no 2nd txn | `CasStore.DropNamespaceRemovesEveryOwnerButLeavesFilesForGc` (idempotent repeated drop) |
| 11 | birth without marker rejected (empty prefix) | `RefWriterNamespaceBirth.BirthFromRemovedRejectedWithoutMarkerAcceptedWithMarker` |
| 11 | birth with marker accepted, continues timeline | same |
| 12 | >1000-key scan folds each log once | `CasRefGc.LargeRefScanFoldsEveryLogExactlyOnce` (N=1200, multi-page) |
| 12 | concurrent append not skipped (cursor below) | `CasRefGc.ConcurrentLogAfterScanIsFoldedNextRound` |
| 12 | cursor never past unreturned log (fault-inj pagination) | **weakened** — see note |
| 12 | edge cancellation | `CasRefGc.EdgeCancellationAddThenRemoveReclaimsBlob` |
| 12 | losing commit adopts/deletes nothing | `CasRefGc.LosingGenerationCommitAdoptsNothingDeletesNothing` |
| 12 | cleanup respects all 3 conditions | `CasRefGc.RefObjectCleanupHonorsAllThreeConditions` + `CasRefIntake.PlanRefCleanupThreeConditions` |
| 12 | item re-executed after leader change | `CasRefGc.RemoveNamespaceCompletesAndPublishesMarkerDeterministically` |
| 12 | marker + Removed-snapshot republication | same |
| 12 | sweep protects tail-removed, skips on bad input | `CasOrphanManifestSweep.{PendingCommittedRemovalBodyIsSkipped, OwnedBodyIsSkipped, NoWatermarkIsNotAuthority}` |
| 12 | malformed key/body aborts fold | `CasRefGc.MalformedRefKeyAbortsRefFoldingNoPartialDelta` |
| 12 | (fold barrier) missing body clamps | `CasRefGc.FoldBarrierClampsBelowMissingBodyThenFoldsOnAppear` |
| 13 | fsck cache-replay/snapshot byte-compare oracle | `gtest_cas_fsck.cpp` (`snapshot_oracle_checked`) |
| 13 | counters | `CasRefGc.RefIntakeIncrementsObservabilityCounters` + `RefWriterSnapshotPublish.PublishIncrementsSnapshotCounters` |
| 13 | e2e | `CasRefGc.RefSnaplogLifecycleE2E` |

**Note — Task 12 "cursor never advances past an unreturned log (fault-injected pagination)":** there is
no C++ mid-scan pagination fault-injection test. The safety premise is carried by (a) the deterministic
multi-page `LargeRefScanFoldsEveryLogExactlyOnce` (1200 keys → real multi-page LIST, every log folded
exactly once, cursor at the greatest), (b) `ConcurrentLogAfterScanIsFoldedNextRound` (a post-scan append
stays below the sealed cursor, folded next round), and (c) the stronger TLA+ gate
`CaRefDeltaIntakeCore` (Task 2), whose `_safe` config proves the three-premise cursor-safety and whose
sabotage toggle "resume beyond the last returned key" fails. Accepted: the fault-injection dimension is
model-checked, the C++ layer witnesses the deterministic case. Not a gap.

All other Tasks 4-13 bullets have a live, asserting HEAD test. Two scope items lack a test and are
closed in Part 4: GAP-B (one-op-per-ref batch cut) and GAP-A (baseline guard, from Part 1).

## Part 2 — Structural coverage

Dedicated coverage build `build_cov` (clang-21 source-based `-fprofile-instr-generate
-fcoverage-mapping`, **RelWithDebInfo** to match the regular `build` — see note), full CAS sweep
(`*Cas*:RefWriter*:*RefTableCache*`, 825/825), `llvm-cov` per-file (`cas.profdata` 2.6 MB from a
174 MB profraw).

**Build-type note (important):** a first coverage build used `CMAKE_BUILD_TYPE=Debug`, which defines
`DEBUG_OR_SANITIZER_BUILD`, turning `LOGICAL_ERROR` fail-closed tests into process `abort()`s — the
sweep aborted at `CasSourceEdgeRun.SourceEdgeIdZeroIsReserved` and wrote zero coverage. Rebuilt as
RelWithDebInfo (NDEBUG, `abort_on_logical_error=false`, matching the 820/0 regular build) so
`LOGICAL_ERROR` tests throw catchably and the fail-closed paths are measured rather than aborting.

### Per-file coverage (new/rewritten Core files)

| File | Region % | Line % | Branch % |
|---|---|---|---|
| `CasRefIds.h` | 100.00 | 100.00 | 100.00 |
| `CasRefLogCodec.cpp` | 97.06 | 98.44 | 94.64 |
| `CasRefSnapshotCodec.cpp` | 100.00 | 100.00 | 98.00 |
| `CasRefStateMachine.cpp` | 99.31 | 99.53 | 92.86 |
| `CasRequestControl.cpp` | 84.47 | 98.54 | 76.53 |
| `CasRefIntake.cpp` | 92.00 | 90.54 | 87.21 |
| `CasGc.cpp` | 64.21 | 90.06 | 73.21 |
| `CasStore.cpp` | 71.69 | 84.60 | 69.05 |
| `CasFsck.cpp` | 82.31 | 86.68 | 72.17 |
| `CasInspect.cpp` | 57.53 | 62.15 | 47.89 |
| `CasOrphanManifestSweep.cpp` | 44.21 | 77.50 | 51.69 |
| `CasLayout.h` | 93.67 | 93.89 | 80.22 |
| `CasGenerationSeal.cpp` | 95.65 | 97.12 | 95.83 |

The transcription-class files (`CasRefIds`, both codecs, `CasRefStateMachine`) are 97-100% line. The
lower region/branch numbers on `CasGc`, `CasStore`, `CasInspect`, `CasOrphanManifestSweep` are **surface
the unit sweep does not reach**, not untested logic: the CLI rendering in `CasInspect` (exercised by the
`clickhouse-disks ca-inspect` integration path), the cursor-page/list-budget arms of the orphan sweep,
and the multi-process mount-fencing / GC-lease-steal / self-remount arms of `CasStore`/`CasGc` (exercised
by ca-soak). `CasInspect` also has 10 decoder overloads only reached from the CLI.

### Uncovered fail-closed / clamp / fence / wedge / restart-on-vanish branches — disposition

Every uncovered line carrying a fail-closed keyword (`throw`/clamp/fence/wedge/restart/abort/refuse) was
enumerated via `llvm-cov show … -show-line-counts-or-regions`. Disposition:

**Closed with a new gap test (5):**

| Uncovered branch | Test |
|---|---|
| `CasGc.cpp:931` baseline guard (data-loss refusal) | GAP-A `CasRefGc.BaselineGuardRefusesWhenSnapshotSurvivesWithoutLogsOrCursor` |
| `CasStore.cpp:1399` one-op-per-ref batch cut | GAP-B `RefWriterAppendLane.SameRefMutationsSplitAcrossFlushes` |
| mountpoint get/remove roundtrip | GAP-C `CasStore.MountpointObjectRoundTrip` |
| `CasRefLogCodec.cpp:79` decode unknown owner kind | GAP-D `CasRefCodec.DecodeRejectsUnknownOwnerKind` |
| `CasGc.cpp:967-971` fold abort on invalid ref log **body** | GAP-E `CasRefGc.InvalidRefLogBodyAbortsFoldNoPartialDelta` |

**Justified — defensive-impossible** (exhaustive-switch-then-throw, reachable only via an out-of-range
`static_cast` of a validated enum; the comment says so at each site): `CasRefLogCodec.cpp:115` (writeOp
unknown op kind), `CasRefStateMachine.cpp:170` (apply unknown op kind). Testing these needs UB to
construct.

**Justified — integration / ca-soak-only** (multi-process or lease-timing paths a single-process unit
test cannot drive; exercised by the integration harness and the soak): repeated GC-fence-out during open
`CasStore.cpp:361-366`; mount double-start `:375`; object-CAS contention / live-lock brake `:483,:507`;
self-remount recovery `:644`; GC-round lease/CAS aborts `CasGc.cpp:509,516,557`; orphan-sweep
skip-on-error and cursor-discarded-because-gc-state-moved `:647,:1263`; rebuild refusal under a competing
writer `:2054`.

**Justified — covered-family / bounded-defensive:**
- restart-on-vanish **exhaustion** throws (`CasStore.cpp:968`, `CasRefIntake.cpp:204`) — the success
  restart path is tested (`RefWriterRecovery.RestartOnVanishConvergesOnNewerSnapshot`); the throw fires
  only after a bounded number of restarts under an adversary that vanishes a selected object *every*
  round, which a deterministic unit test cannot stage; the bound is config-capped and fail-closed.
- admission-budget writer refuse (`CasStore.cpp:1460`) — the `admits` predicate itself is exhaustively
  tested (`CasRefStateMachine.AdmitsRejectsGrowthPast{SnapshotBudget…,RemovalBudget}`); the writer
  wiring is a thin `if (state_growing && !admits(…)) throw` that the sweep never trips because the
  default budgets are large.
- 404-race record-and-continue (`CasGc.cpp:666`) — the never-throw-on-404 invariant
  ([[feedback_ca_gc_never_throw_on_404]]); a `return false` record-and-continue on a HEAD/GET race,
  exercised under soak, never a unit-deterministic event.
- writer DefiniteFailure/Unresolved wedge arms (`CasStore.cpp:1572,1723`) — the outcome classification is
  covered by `CasRequestControl` (`DefiniteFailurePropagatesImmediatelyWithoutResolve`, the Unresolved
  cases) and the wedge behavior by `RefWriterAppendLane.WedgedLaneBlocksSameTableWhileOtherTableProceeds`.

No uncovered fail-closed branch is left both unit-testable and unjustified.

## Part 4 — Gap-filling tests

Five tests added, each shown to discriminate (fails under a targeted mutation of the code it guards,
passes on the clean tree). Full CAS sweep after adding all five: **825 passed / 0 failed** (820 baseline
+ 5), no name-set regressions.

| Gap | New test (file) | Guards | Discriminating mutation | Result under mutation |
|---|---|---|---|---|
| GAP-A | `CasRefGc.BaselineGuardRefusesWhenSnapshotSurvivesWithoutLogsOrCursor` (`gtest_cas_ref_gc.cpp`) | baseline guard, `CasGc.cpp:931` (spec §Offline Recovery) — refuse a fold that would mass-condemn a table whose covered logs vanished with no sealed cursor | `if (logs_below_snapshot_gone)` → `… && false` | FAILS: no `CORRUPTED_DATA` thrown, table B's blob condemned |
| GAP-B | `RefWriterAppendLane.SameRefMutationsSplitAcrossFlushes` (`gtest_cas_ref_writer.cpp`) | one-op-per-ref batch cut, `CasStore.cpp:1399` (`seen_refs` / `CasRefBatchScopeCuts`) | disable the `seen_refs` cut (`… && false`) | FAILS: two same-ref ops merge into one create (`putTotal` 8 vs expected 9) |
| GAP-C | `CasStore.MountpointObjectRoundTrip` (`gtest_cas_store.cpp`) | mount access-check probe get/remove roundtrip | `removeMountpointObject` body → no-op (`(void)key;`) | FAILS: object still present after remove (`gtest_cas_store.cpp:1208-1209`) |
| GAP-D | `CasRefCodec.DecodeRejectsUnknownOwnerKind` (`gtest_cas_ref_codecs.cpp`) | decode-time reject of a corrupt owner-kind byte, `CasRefLogCodec.cpp:79` | whitelist the test sentinel (`… && kind_raw != 99`) | FAILS: 99 accepted, decode returns without throwing |
| GAP-E | `CasRefGc.InvalidRefLogBodyAbortsFoldNoPartialDelta` (`gtest_cas_ref_gc.cpp`) | fold abort on an undecodable ref-log **body** at a valid key, `CasGc.cpp:967-971` (spec §Step 2) | `ref_folding_aborted = true` → `false` in the invalid-body catch | FAILS: partial delta adopted, `inDegree` 1 vs expected 0, cursor advances |

(GAP-A/B/C/E used `… && false`-style neutralizations where the guard variable stays referenced;
`-Wunreachable-code`/`-Wunused` forced the reachable-but-neutralized forms — e.g. GAP-D whitelists the
sentinel rather than `&& false`, and GAP-A's `&&`-of-a-live-bool avoids the unused-variable error.)

Design notes:

- GAP-A was first attempted by driving a fold-then-clean and deleting `gc/state`, but that left the
  fold seal behind and tripped the *seal-divergence* guard (also `CORRUPTED_DATA`), so it did not isolate
  the baseline guard. The committed version instead seeds the exact guard input directly on the first
  round (table B: a surviving `_snap` with no logs and no cursor; healthy table A alongside), so the only
  possible `CORRUPTED_DATA` is the baseline guard — confirmed by the guard's message in the passing run.
- All three mutations were reverted; `grep -rn "DISCRIMINATION MUTATION" src/` is empty and the source
  tree carries only the three test-file additions.

### Micro-items

- (a) ref_gc oracle-SKIP comment reword — **already done at HEAD** (confirmed by the team lead; see the
  Part-1 micro-item note). No change.
- (b) cleanup-backlog gauge (deletable-but-deferred count) — **not taken.** The two deferral points are
  pre-plan early-returns (`Gc::cleanupRefObjects` / `planRefCleanup` return before computing the plan),
  so a deletable-but-deferred count can only be produced by computing the very plans those paths skip —
  not free at the plan level here. Left to the final review as a tracked optional item, per the team
  lead's guidance. None of the three gap tests sit on `cleanupRefObjects`/`planRefCleanup`, so there was
  no natural place to fold it in.

## Concerns / follow-ups

- Vestigial backpressure config: `manifest_soft_limit`/`manifest_hard_limit`/`manifest_max_delay_ms` are
  plumbed disk-config→`PoolConfig` but unused (no delay path reads them). The header comment at
  `CasStore.h:198` still says "root-shard body". Candidate for removal; out of scope here.
