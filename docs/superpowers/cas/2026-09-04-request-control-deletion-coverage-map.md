---
description: 'Per-property coverage map for the 53 tests deleted with gtest_cas_request_control.cpp: which current test covers each deleted test''s property, which are intentionally superseded by a design change, and which are genuine open gaps.'
sidebar_label: 'Request-control deletion coverage map (2026-09-04)'
sidebar_position: 3
slug: /superpowers/cas/request-control-deletion-coverage-map-2026-09-04
title: 'Coverage map: the 53 tests deleted with gtest_cas_request_control.cpp'
doc_type: 'reference'
---

# Coverage map: the 53 tests deleted with `gtest_cas_request_control.cpp` {#coverage-map-request-control-deletion}

`src/Disks/tests/gtest_cas_request_control.cpp` (53 tests) was deleted together with the module it
tested, `CasRequestControl.h` — a standalone pre-migration retry controller (`CasRequestController`,
`Token`, `PutOutcome`, `CasOverwriteOperationContext`, `CasOverwriteProgress`, `CasOverwriteStopCause`,
free-function `validateCasRequestBudget` with `max_attempts`/`operation_deadline_ms`/
`retry_initial_backoff_ms` fields) that the request-contract migration replaced wholesale with
`CasOperation`/`CasRequests` (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/
CasRequests.{h,cpp}`) and the `WriteResult`/`Retry` vocabulary (`CasWriteResult.h`). None of the old
controller's types exist in the tree any more; this table maps each deleted test's PROPERTY — not its
literal assertions, which targeted an API that is gone — to its current covering test, a restored
test, or a stated reason it no longer applies.

Recovered with `git show 88b367a0eaebeea29ee4351ca1b894a42570bd3f^:src/Disks/tests/gtest_cas_request_control.cpp`
(the commit before the file's deletion).

## Classification — mapping an exception/response to Committed / DefiniteFailure(Refused) / Unresolved(ambiguous) {#classification}

The old three-way `classifyConditionalWriteResult` split across two current layers: the S3
write-buffer-level classifier `detail::finalizeConditionalWrite` (`CasObjectStorageBackend.cpp`, only
Applied vs PreconditionLost, rethrowing everything else), and `CasOperation::writeLoop`'s own exception
handling (`isDefinitelyRefusedWrite` for a definite refusal, any `Poco::Exception` folded into ambiguity,
anything else rethrown).

| Deleted test | Current coverage |
|---|---|
| `SuccessIsAlwaysCommitted` | `CASS3Signal.FinalizeClassifierMapsPreconditionLossExactly` (the `Applied` case) |
| `ClassifiesPreconditionFailedAsUnresolved` | `CASS3Signal.FinalizeClassifierMapsPreconditionLossExactly` (`PreconditionFailed` → `PreconditionLost`, this layer's name for "ambiguous, not definite") |
| `ClassifiesTimeoutAsUnresolved` | `CASRequests.ATransportTimeoutIsReissuedAndALocalFailureIsNot` (`Poco::TimeoutException` folds into ambiguity and is reissued) |
| `ClassifiesConnectionResetAsUnresolved` | Same code path as the row above, proven by CLASS (`dynamic_cast<const Poco::Exception *>`), not by exception subtype — `Poco::Net::ConnectionResetException` is a `Poco::Exception` and takes the identical branch. Not separately pinned by subtype; the class-level proof subsumes it. |
| `Classifies5xxAsUnresolved` | **Gap.** No current test constructs an `S3Exception` carrying `SlowDown`/`InternalFailure` on the WRITE path to prove it falls through to ambiguity (rather than `Refused`) the way `isDefinitelyRefusedWrite`'s negative case implies. The mechanism is exercised on the READ path by `AnUnmodeledStoreErrorOnAReadIsReissuedNotSurfaced`; the write-path instance is untested. |
| `ClassifiesMalformedRequestAsDefiniteFailure` | `CASRequests.AMalformedRequestIsRefusedWithoutAReissue` |
| `ClassifiesEntityTooLargeAsDefiniteFailure` | **Gap.** `isEntityTooLargeError` shares `isDefinitelyRefusedWrite`'s code path with `MalformedXML`/`AccessDenied` (same `||` chain inside `isDefinitelyRefusedWrite`), so the MECHANISM is proven by the row above and by `AnAccessDenialNoRefreshCanFixIsRefusedOnTheFirstAttempt`, but no test constructs the `EntityTooLarge` name itself. |
| `ClassifiesAccessDeniedAsDefiniteFailure` | `CASRequests.AnAccessDenialNoRefreshCanFixIsRefusedOnTheFirstAttempt` |
| `UnrecognizedErrorsFailSafeToUnresolved` | `CASS3Signal.FinalizeClassifierMapsPreconditionLossExactly` (an unnamed `SlowDown` is rethrown, not misclassified as `PreconditionLost`) plus `AnUnmodeledStoreErrorOnAReadIsReissuedNotSurfaced` on the read path; the write-path instance shares the gap noted for `Classifies5xxAsUnresolved`. |
| `CountersHookupIncrementsPerClass` | **Obsolete.** `recordConditionalWriteAttemptStarted`/`recordConditionalWriteOutcome`/`classifyConditionalWriteResult` moved into an anonymous namespace inside `CasObjectStorageBackend.cpp` — private implementation detail, unreachable from a unit-test binary without exporting new API solely to satisfy this test. Exercised only indirectly, as a side effect, by any test that performs a real Native write. |
| `NativeConditionalPutCountsOneAttemptAndCommitted` | **Obsolete — behavior changed, not just renamed.** The old test asserted a Native write against a token-less object storage throws `CAS_WRITE_UNATTRIBUTED` immediately. Under the current engine, `CasRequests::tryMint` on an empty/unattributable token value returns `std::nullopt` rather than throwing, and `writeLoop` folds that into an ordinary ambiguous attempt resolved by a read (`CasRequests.cpp:790-793`) instead of surfacing `CAS_WRITE_UNATTRIBUTED` at all. `CAS_WRITE_UNATTRIBUTED` now fires only from the separate emulated-backend `emuMintToken` path. Restoring the old assertion verbatim would pin behavior that no longer holds. |
| `SingleAttemptProfileRequestedAndLocalBackendRejected` | **Restored** as `CASObjectStorageBackend.ConditionalWriteSelectsSingleAttemptAndLocalStorageDoesNotSupportIt` (`gtest_cas_backend.cpp`). |
| `ConditionalWriteSettingsForceSingleUnexpectedWriteErrorRetry` | Same restored test as the row above. |

## Diagnostics of the terminal-give-up throw {#terminal-give-up-diagnostics}

| Deleted test | Current coverage |
|---|---|
| `ThrowsNetworkErrorNotAborted` | `CASWriteResult.OrThrowMapsEveryAlternative` — every `GaveUp` variant (`Deadline`, `Unresolved`, `FenceLost`) maps to exactly `NETWORK_ERROR`, never `ABORTED`, via the current `orThrow`/`throwCasWriteRetryLater` path. |
| `ExceptionPtrVariantCarriesSameClassification` | **Minor gap, lower risk than the old test implies.** `makeCasWriteRetryLaterExceptionPtr` (`CasRequests.h:73`) still exists and is still used by `CasRefLedger`'s queued-append completion paths; its own doc comment states both entry points route through the SAME construction internally, so the classification cannot drift between them by construction, not just by convention — but no direct unit test proves it. |

## Ambiguity settlement — an ambiguous attempt resolved by an exact read, then reissued or claimed {#ambiguity-settlement}

The old `Token`/`PutOutcome`-shaped API (`UncertainResolves*`, `OverwriteAmbiguousResolves*`) is now the
`Etag`-shaped `create`/`replace` primitives with `WriteResult` outcomes (`Committed`/`Conflict`/`GaveUp`).

| Deleted test | Current coverage |
|---|---|
| `UncertainResolvesIdenticalAsCommitted` | `CASRequests.AmbiguousCreateThatLandedIsCommittedByTheResolveRead` |
| `UncertainResolvesDifferentThrowsCorruption` | **Intentional behavior change, not a gap.** The old controller threw `CORRUPTED_DATA` directly on a mismatched resolve; the current engine returns an ordinary `Conflict` instead and lets the caller decide (`CASRequests.EveryConflictIsSettledByOneReadAndCarriesTheOccupant`, `AmbiguousReplaceWhoseResolveShowsAnotherIncarnationIsAConflict`) — an automatic throw from inside the primitive was the old design; the new one reports and defers. |
| `UncertainResolvesAbsentRetriesSameKeyWithinBudget` | `CASRequests.AmbiguousCreateThatNeverLandedIsReissued` |
| `OverwriteAmbiguousResolvesIntendedBytesAsCommitted` | **Intentional behavior change, not a gap — the new rule is STRICTER.** The old controller claimed a resolve that found its own intended bytes as `Committed` by byte equality alone. `CASRequests.AmbiguousReplaceOfIdenticalBytesIsReissuedNotClaimedByByteEquality` deliberately REPLACES that rule: byte equality alone no longer proves the attempt's own authorship (a concurrent writer's identical bytes would otherwise be wrongly claimed), so the current engine reissues instead of claiming. |
| `OverwriteAmbiguousResolvesExpectedTokenAndRetriesWithinBudget` | `CASRequests.AmbiguousReplaceWhoseResolveShowsThePreconditionUnchangedIsReissued` |
| `OverwriteAmbiguousResolvesDifferentTokenAndBytesAsConflict` | `CASRequests.AmbiguousReplaceWhoseResolveShowsAnotherIncarnationIsAConflict` |
| `MaxAttemptsOneStillResolvesLostResponseByGet` | `CASRequests.OnceSendsOneWriteAndAtMostOneResolveRead` |

## Deadlines {#deadlines}

| Deleted test | Current coverage |
|---|---|
| `OperationDeadlineExhaustionReturnsUnresolvedBeforeMaxAttempts` | `CASRequests.DeadlineIsTheOnlyBoundUnderZeroLatencyThrottling` |
| `EqualAttemptTimeoutAndDeadlineWouldRefuseAfterASingleTick` | The specific field pair (`attempt_timeout_ms == operation_deadline_ms`) is gone with the old `CasRequestBudget`. The surviving mechanism — the pre-send gate refusing to start an attempt the deadline cannot afford — is `CASRequests.TheGateBeforeTheSleepEndsTheCallWithoutASecondWrite` and `AWriteReservesTwoEnvelopesSoOneOfSurplusStartsNothing`. |
| `ValidateBudgetRejectsAttemptTimeoutEqualToOperationDeadline` | **Obsolete field.** `operation_deadline_ms` does not exist on the current `CasRequestBudget` (see `CasRequestBudget.h`) — deadlines are now owned by the `Retry` policy, not the budget struct. |
| `OverwriteOperationDeadlineExhaustionReturnsUnresolvedBeforeMaxAttempts` | Same as `OperationDeadlineExhaustionReturnsUnresolvedBeforeMaxAttempts` above — `create` and `replace` share one `writeLoop`. |
| `AbsoluteDeadlineCannotBeReanchoredAfterPreemption` | `CASRetry.AFrozenPolicyIsOneDeadlineAndTheLeaseStillWins` (`op.freeze` fixes ONE absolute deadline for the whole call). |
| `CompletedWaitCrossingDeadlineSendsNoRetry` | `CASRequests.TheGateBeforeTheSleepEndsTheCallWithoutASecondWrite` |
| `DirectPutCompletingAtDeadlineIsNotAccepted` | `CASRequests.AWriteReservesTwoEnvelopesSoOneOfSurplusStartsNothing` |
| `ReadProofCompletingAtDeadlineIsNotAccepted` | `CASRequests.LeaseBoundPolicyIssuesNothingPastTheBoundary` |
| `ResolveFailuresExhaustDeadlineWithoutSendingLatePut` | `CASRequests.AConflictWhoseResolveReadFailsIsReportedWithNothingObserved` |

## Fencing {#fencing}

| Deleted test | Current coverage |
|---|---|
| `FenceLostBeforeAttemptSendsNoAttempt` | `CASRequests.AFenceWithNoBudgetForTheRequestSendsNothingAndNamesTheLease`, `AdmissionIsCheckedAtThreePoints` |
| `FenceLostAfterWriteNeverReturnsCommitted` | `CASRequests.AFenceLostDuringTheResolveReadIsAFenceLossNotAConflict` |
| `FenceLostBeforeSleepAbortsWithoutSleeping` | `CASRequests.TheGateBeforeTheSleepEndsTheCallWithoutASecondWrite` |
| `FenceWinsCancellationAndExternalDeadlineWinsDeadlineTie` | The old caller-supplied `stop_cause`/liveness-predicate hook survives as the current caller-supplied liveness predicate: `CASRequests.LivenessPredicateEndsTheOperationLikeAFenceLoss` plus `AdmissionIsCheckedAtThreePoints` (the fence check now happens at exactly three fixed points instead of an arbitrary cancellation callback). |

## Backoff {#backoff}

| Deleted test | Current coverage |
|---|---|
| `CappedExponentialSleepsAreFenceCheckedAndOrdered` | `CASRetry.BackoffIsFullJitterUnderTheCap` (the shape) plus `CASRequests.TheGateBeforeTheSleepEndsTheCallWithoutASecondWrite` (fence checked before the sleep). |
| `SleepThatWouldCrossOperationDeadlineIsSkipped` | `CASRequests.AWriteReservesTwoEnvelopesSoOneOfSurplusStartsNothing`, `DeadlineIsTheOnlyBoundUnderZeroLatencyThrottling` |
| `DefaultBudgetRidesSixtySecondOutage` | `CASRetry.PoliciesAreShapedAsSpecified` — pins `Retry::standard()`'s own shape/window directly; the old test's arithmetic pinned the same shape indirectly through budget fields that no longer exist. |

## Startup validation {#startup-validation}

`validateCasRequestBudget` still exists (`CasRequestBudget.h`/`.cpp`), but its current `CasRequestBudget`
has only two of the old fields (`attempt_timeout_ms`, `lease_safety_margin_ms`); `max_attempts`,
`operation_deadline_ms`, `retry_initial_backoff_ms`, `retry_max_backoff_ms` are gone — retry attempts,
deadlines and backoff shape are now owned entirely by the fixed-shape `Retry` policy type
(`Retry::standard()`/`Retry::once()`/`Retry::backoff()`), which cannot be constructed inconsistently the
way a free-form budget struct could, obsoleting the need to validate those fields at startup at all.

| Deleted test | Current coverage |
|---|---|
| `ValidateBudgetAcceptsConsistentDefaults` | **Restored** as `CASRequestBudget.ValidateAcceptsDefaultsAndRejectsAnOverflowingSumWithoutWrapping` (`gtest_cas_mount.cpp`). |
| `ValidateBudgetRejectsAttemptTimeoutPlusMarginAtOrAboveLeaseTtl` | The one inequality that survives (`attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms`) is covered end-to-end by `CASMountStartup.RefusesWritableOpenWithInconsistentCasRequestBudget` (the exact `==` boundary) and directly, in isolation, by the restored test above. |
| `ValidateBudgetRejectsAttemptTimeoutAboveOperationDeadline` | **Obsolete field** (`operation_deadline_ms` gone; see Deadlines). |
| `ValidateBudgetRejectsZeroMaxAttempts` | **Obsolete field** (`max_attempts` gone; a `Retry` policy is a fixed, always-valid shape). |
| `ValidateBudgetRejectsInitialBackoffAboveMaxBackoff` | **Obsolete field** (`retry_initial_backoff_ms`/`retry_max_backoff_ms` gone; `Retry::backoff`'s cap relationship is pinned by construction and covered by `CASRetry.PoliciesAreShapedAsSpecified`). |
| `ValidateBudgetRejectsOverflowingSumRatherThanWrapping` | **Restored** as `CASRequestBudget.ValidateAcceptsDefaultsAndRejectsAnOverflowingSumWithoutWrapping` (`gtest_cas_mount.cpp`) — the current implementation still guards this exact overflow (subtraction against the TTL rather than a direct sum), and the restored test proves it against a near-`UINT64_MAX` config. |

## Progress/stop-cause diagnostics apparatus — entirely obsolete {#progress-stop-cause-obsolete}

`CasOverwriteStopCause`, `CasOverwriteProgress`, `CasOverwriteProgressKind`, `CasUnresolvedReason` and the
`CasOverwriteOperationContext` observer/stop-cause hooks these six tests exercised do not exist anywhere
in the current tree (verified: `grep -rn` over `src/Disks/` outside `tests/` finds none of them). The
caller-supplied liveness predicate and the fence check that replaced this whole apparatus are covered by
`CASRequests.LivenessPredicateEndsTheOperationLikeAFenceLoss` and `AdmissionIsCheckedAtThreePoints`.

| Deleted test | Disposition |
|---|---|
| `StopBeforeFirstPutReportsExactCause` | Obsolete — no `stop_cause`/progress apparatus in the current engine. |
| `StopAfterPutSuppressesResolveAndReportsMidWay` | Obsolete, same reason. |
| `StopAfterResolvedCommitReportsPostWrite` | Obsolete, same reason. |
| `InterruptedWaitResamplesStopCause` | Obsolete — no "interrupted wait" concept to resample; the current fence check is synchronous at three fixed points, not a waited/interruptible state. |
| `InterruptedWaitWithoutPublishedStopIsAProgrammingException` | Obsolete, same reason. |
| `InterruptedWaitWithoutPublishedStopIsAProgrammingExceptionAborts` (death-test twin) | Obsolete, same reason. |
| `ObserverFailureCannotChangeOutcome` | Obsolete — no progress-observer hook exists to fail. |
| `EveryTerminalShapeReportsExactDiagnostics` | Its SPIRIT (every terminal outcome shape maps to an exact, exhaustively-enumerated diagnostic) is the current `CASWriteResult.OrThrowMapsEveryAlternative`. |
| `DefiniteFailurePropagatesImmediatelyWithoutResolve` | `CASRequests.AMalformedRequestIsRefusedWithoutAReissue` (a refusal is terminal, no resolve read is sent). |

## Summary {#summary}

Counting every one of the 53 rows across the tables above by disposition:

- **4** covered by a test **restored** this round against a current-vocabulary equivalent
  (`CASRequestBudget.ValidateAcceptsDefaultsAndRejectsAnOverflowingSumWithoutWrapping` and
  `CASObjectStorageBackend.ConditionalWriteSelectsSingleAttemptAndLocalStorageDoesNotSupportIt`, two new
  tests each covering two old test names).
- **32** have a direct or intentionally-superseding current covering test.
- **7** are obsolete because the whole `CasOverwriteStopCause`/`CasOverwriteProgress` apparatus they
  exercised no longer exists anywhere in the tree.
- **4** are obsolete because the specific `CasRequestBudget` field they validated (`operation_deadline_ms`,
  `max_attempts`, `retry_initial_backoff_ms`/`retry_max_backoff_ms`) no longer exists, superseded by the
  fixed-shape `Retry` policy type.
- **2** are obsolete for a different reason each: `CountersHookupIncrementsPerClass`'s subject moved into
  an anonymous namespace (private implementation detail), and `NativeConditionalPutCountsOneAttemptAndCommitted`
  pinned a behavior (`CAS_WRITE_UNATTRIBUTED` on an unattributed Native write) that the engine no longer
  exhibits at all.
- **4 are genuine, still-open gaps**: `Classifies5xxAsUnresolved` and `UnrecognizedErrorsFailSafeToUnresolved`'s
  write-path instance (an `S3Exception` that is neither definitely-refused nor Poco-classified falling
  through to ambiguity is untested on the write path — only on the read path), `ClassifiesEntityTooLargeAsDefiniteFailure`
  (the `EntityTooLarge` name specifically, though its code path is proven by name-adjacent tests), and
  `ExceptionPtrVariantCarriesSameClassification` (`makeCasWriteRetryLaterExceptionPtr`'s own
  classification, unverified in isolation though guaranteed by construction per its own doc comment).
  These four are real but narrow — each shares its code path with an already-tested sibling — and are
  recorded in `docs/superpowers/cas/BACKLOG/testing-and-ci.md` (`{#request-control-classification-gaps}`)
  rather than closed under time pressure with an unverified test.
