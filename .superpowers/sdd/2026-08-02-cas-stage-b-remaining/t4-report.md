# Task T4: Task 8 closure — duty queue and orphan nomination

Status: **DONE**. All seven plan steps executed. Full release CA gate and ASan CA gate both green;
S3 integration lanes green.

## Step 1 — settlement ordering (pin, not a fix)

Read `Pool::drainWriterCleanupDuties` (`Pool/CasPool.cpp`): the ordering already matches the Q-1
decision exactly — append the exact `OwnerTransition` removal (or observe conclusive absence) via
`ref_ledger.appendRefOps`, only then `retireBuildSeq(duty->build_seq)`, only then `pop_front()` the
duty. The `catch (...)` arm restores `draining = false` without popping the duty and without having
called `retireBuildSeq`, so a throw between those steps retains the duty for retry. No code change
was needed; added `CasWriterDuties.DutySurvivesSettlementFailureForRetry` as the pin, driven
red-first is not applicable here (no code changed) but the test itself was written and run against
the pre-existing code with a genuine fault injected on the SETTLEMENT's own append
(`ChunkFaultBackend::Mode::Unresolved`, distinguished from the ORIGINAL grant's own append by using a
plain, unfaulted `Durable` precommit as the inherited duty). Log: `build/t4_new_tests_final.log`
(part of the combined run).

## Step 2 (T-2) — reject-arm wedge drain

Added `CasWriterDuties.WedgeResolvedAsRejectDrainsTheDutyAsNoOp`: drives an uncertain grant into an
ACTUAL wedged lane via `ChunkFaultBackend::Mode::Unresolved` (distinct from
`ProvenAbsentGrantDrainsAsNoOpBeforeTheNextMutation`'s controller pre-attempt refusal, which never
wedges at all), then resolves it as REJECT via the next attempt's resolve-before-reissue GET proving
the key absent. Asserts the duty then drains as a no-op and `minActive` advances past the rejected
build. Red-first: this scenario had model coverage only before this step; the test was written fresh
and passes against the existing (correct) production code.

## Step 3 (T-1) — tighten the loose fences

`gtest_cas_orphan_nomination.cpp`'s `CorruptManifestIsRetainedAndSurfaced` and
`TokenAbaIsRetainedAndSurfaced` now use `expectThrowsCode(ErrorCodes::CORRUPTED_DATA, ...)` instead of
`EXPECT_THROW(..., DB::Exception)`. Both throw sites (`CasOrphanManifestSweep.cpp`'s manifest-identity
check; `CasGc.cpp`'s ABA path in `orphan_sweep`) are `CORRUPTED_DATA`, not `LOGICAL_ERROR`, so no
death-test split is needed.

## Step 4 (T-3) — accounting on the real round

`RetiresExactManifestSourcesBeforeDelete` now captures the nominating round's `fold_reduce`
`GcPhaseRecord` (via `Gc::setPhaseSink`) and asserts `metrics.at("unmatched_removes") == 0` and
`metrics.at("txns_unapplied") == 0` — the same two numbers `SourceRetirementIsAccountingNeutral`
proves on a synthetic `foldDeltasIntoGeneration` call, now proven on the real end-to-end round.

## Step 5 (C-1) — remove the footgun from production

`sweepManifestCursorPage` moved out of `CasOrphanManifestSweep.{h,cpp}` into a new
`src/Disks/tests/cas_sweep_test_support.h` (`sweepManifestCursorPageForTest`), included by
`gtest_cas_orphan_manifest_sweep.cpp` and `gtest_cas_sweep_deletion_premise.cpp` (their only two
callers, both tests). Production TUs compile without it.

```
$ git grep -n "sweepManifestCursorPage" src/ | grep -v tests
<no output, exit 1>
```

## Step 5b — Q-1 acceptance mapping

1. **Rejected debris does not leak forever.** Binding half: Step 2's reject-arm test + Step 1's
   retention-on-failure pin. **Physical half**: added
   `CasWriterDuties.RejectedAttemptBodyIsEventuallyNominatedAndSwept` — a real predecessor/successor
   crash scenario (mirroring `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant`'s
   pattern) where the predecessor's precommit is genuinely REJECTED (`ChunkFaultBackend::Mode::Unresolved`
   — nothing lands, unlike the ADOPT-arm's `LandedThenLost`), the successor's own recovery closes the
   dead epoch with an arithmetic seal, and the rejected manifest's body is confirmed physically deleted
   after real `Gc` rounds. Building this test surfaced a real test-authoring gotcha (see "Notable
   debugging" below), now fixed.
2. **Eventual nomination for Uncertain/Durable bodies.** Unchanged from the audit's citation:
   `RetiresExactManifestSourcesBeforeDelete` (real-round nomination) +
   `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant` (writer-abandoned Durable remnant
   reaches the sweep via the successor's crash recovery). Both already existed and both pass.
3. **Missing attribution → suppression, never destructive fallback.** Cited in code:
   `Gc::fold` (`CasGc.cpp`) gates orphan-sweep PLANNING itself on
   `if (!suppress_destructive && manifest_sweep_list_budget_keys > 0)` — a suppressed pass never even
   calls `planManifestCursorPage`. Added the positive observation:
   `CasOrphanNomination.SuppressedRoundNominatesNothing` drives `gc.runRegularRound()` with NO third
   argument (the production default, `UniversePolicy::kDefault`, i.e. suppressed) over the same
   S42-shaped orphan fixture `RetiresExactManifestSourcesBeforeDelete` uses, and asserts (via the
   `orphan_sweep` phase record) `listed == 0`, `deleted == 0`, `suppressed == 1`, and that the
   candidate manifest survives.
4. **Accounting names each debris class's owner.** Step 4's real-round accounting assertions + Step 1's
   pin.

## Step 6 — mutation demonstrations

Load-bearing mutation demonstration performed after implementation; mutation reverted; patch and
failing output preserved (see below and in this report). All three probes: apply → build → run
target test(s) → capture red → revert → confirm build clean again.

1. **Drop `mutateRefsAfterWriterCleanup` from `dropRef`.** Mutation: `Pool::dropRef` calls
   `ref_ledger.dropRef(ns, ref_name)` directly. Result: `CasWriterDuties.DropRefServicesPendingDutyBeforeRemovingTheRef`
   fails exactly as the plan predicts (`minActive() == 2` vs `peekNextBuildSeq() == 3`). Reverted;
   confirmed green again.
2. **Make the writer-duty transfer skip the `Uncertain` branch.** Mutation:
   `PartWriteTxn::~PartWriteTxn`'s condition narrowed to `precommit_state == PrecommitState::Durable`
   only (dropping `Uncertain`). Result: **7 tests fail** — `UncertainAdoptedGrantStaysActiveUntilTheNextMutationRemovesIt`,
   `ProvenAbsentGrantDrainsAsNoOpBeforeTheNextMutation`, `WedgeResolvedAsRejectDrainsTheDutyAsNoOp`,
   `DropRefServicesPendingDutyBeforeRemovingTheRef`, `UpdateRefPublishedAtServicesPendingDutyBeforeUpdatingTheRef`,
   `DropNamespaceOverloadsServicePendingDutyBeforeRemoval`,
   `SnapshotAttemptServicesPendingDutyBeforePublishingLedgerState`. **Deviation from the plan's wording,
   disclosed rather than buried**: the plan named
   `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant` as the expected failing test for this
   mutation. That test's "abandoned" precommit reaches `PrecommitState::Durable` via a plain,
   unfaulted `precommitAdd` (no wedge at all) — it is a `Durable`-branch scenario, not an
   `Uncertain`-branch one, so narrowing the condition to `Durable` alone does not touch it; it (and the
   two new physical-sweep tests, which are also `Durable`-shaped) still pass under this mutation. The
   seven tests actually broken by this exact mutation are the correct, precise demonstration of the
   `Uncertain` branch's load-bearing-ness; the plan's named test was simply the wrong pointer, not a
   gap in coverage. Reverted; confirmed green again.
3. **Skip source-edge retirement in the nomination path.** Mutation: `planManifestCursorPage`'s
   `source_retirements` derivation loop deleted (nomination pushed with an always-empty vector).
   Result: `CasOrphanNomination.RetiresExactManifestSourcesBeforeDelete` fails on
   `source_absent_when_delete_started`, `activeSourceExists`, `inDegreeInRuns`, and `condemnedCount` —
   four independent assertions, all as expected. Reverted; confirmed green again.

After each mutation, `unit_tests_dbms` was rebuilt and the specific target test(s) run to confirm red;
after all three were reverted, a full rebuild + the entire filtered gate was re-run to confirm the
tree is clean again (see gate numbers below — the SAME 1985/1985 both before and after the mutation
round).

## Notable debugging: a test-authoring bug, not a production bug

Constructing `RejectedAttemptBodyIsEventuallyNominatedAndSwept` initially produced a manifest that
was retained forever despite satisfying both premise rules (build epoch strictly below the folded
cursor's epoch; not in the recovered ref table's committed/precommit view; not in any unconsumed
tail-removal set — verified directly by instrumenting `namespaceFoldView`,
`recoverRefTableDetailedAtCatalogCutForTest`, and the raw `_ckpt`). The actual cause: the test's
namespace was rooted under a DIFFERENT server-root prefix (`"srv1/..."`, copied from this file's other
writer-duty fixtures, none of which exercise the orphan sweep) than the `Pool`'s own
`server_root_id` (`"test"`). `CasOrphanManifestSweep.cpp`'s `prefixEligible` derives its watermark
floor by walking the NAMESPACE's own prefix segments for a live mount lease
(`floorForNamespace`) — with no mount lease ever written under `"srv1"`, every build prefix under
that namespace is permanently `NOT ELIGIBLE`, independent of epoch/coverage. Renaming the namespace to
`"test/writer_duty_rejected_sweep"` (matching the pool's real server-root) fixed it immediately. No
production defect. Filed here because it cost significant investigation time and is a trap other CAS
gtest authors reusing this file's fixtures could fall into.

A second, unrelated hazard surfaced during this session: rebuilding `build_asan` in the background
concurrently with source-level mutation edits on `build`'s shared source tree let the ASan binary pick
up an intermediate (mutated) state of `CasPool.cpp` at whatever point ninja's compile step for that
translation unit happened to run. The first ASan gate attempt showed 2 spurious failures
(`DropRefServicesPendingDutyBeforeRemovingTheRef`, `DutySurvivesSettlementFailureForRetry`) that did
not reproduce against a verified-clean full rebuild. Fixed by never running a background build of one
binary while foreground-editing source for another binary's mutation probes; the final ASan gate ran
against a `build_asan` rebuilt strictly after all three mutations were reverted.

## Step 7 — gates + closure

- **Full release CA gate**: `utils/cas-gate/generate_cas_suites.sh build` → 278 suites (0 unclaimed).
  `build/src/unit_tests_dbms` with the generated filter: **1985 ran / 1985 passed / 0 failed**, 278
  suites. Log: `build/t4_gate_final.log`. This is 4 more than the audit's cited pre-T4 baseline of
  1980 (the four new tests this task adds: `WedgeResolvedAsRejectDrainsTheDutyAsNoOp`,
  `RejectedAttemptBodyIsEventuallyNominatedAndSwept`, `DutySurvivesSettlementFailureForRetry`,
  `SuppressedRoundNominatesNothing`); no unexplained delta.
- **Full ASan CA gate**: `build_asan/src/unit_tests_dbms` with the same filter: **1957 ran / 1957
  passed / 0 failed**, 278 suites, 0 sanitizer reports, 0 aborts (the `ABORTED` string hits in the log
  are pre-existing `ErrorCodes::ABORTED` scenario text from unrelated tests, not process aborts).
  1957 vs 1985 under release reflects existing sanitizer-conditional test gating elsewhere in the
  suite (not introduced by this task). Log: `build/t4_asan_gate2.log`.
- **S3 integration lanes**:
  `python3 -m ci.praktika run "integration" --test "test_content_addressed_s3 test_content_addressed_gc_s3"`
  → **3 passed** (`test_content_addressed_s3::test_content_addressed_s3`,
  `test_content_addressed_s3::test_mutations_and_patch_parts_survive_restart`,
  `test_content_addressed_gc_s3::test_stage_a_gc_is_suppressed_and_says_so`), 0 failed. Log:
  `build/t4_s3.log`.

## Files changed

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.{h,cpp}`
  (Step 5: `sweepManifestCursorPage` removed)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (Step 1's test seam:
  `writerCleanupDutiesPendingForTest`)
- `src/Disks/tests/gtest_cas_writer_duties.cpp` (Steps 1, 2, 5b: three new tests)
- `src/Disks/tests/gtest_cas_orphan_nomination.cpp` (Steps 3, 4, 5b: two tightened fences, real-round
  accounting, one new test)
- `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp`,
  `src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp` (Step 5: call-site rename to
  `sweepManifestCursorPageForTest`)
- `src/Disks/tests/cas_sweep_test_support.h` (new, Step 5)

## Deviations from the brief, disclosed

- Step 6, mutation 2: the plan named the wrong test as the expected failure (see "Mutation
  demonstrations" above) — the underlying claim (the `Uncertain` branch is load-bearing) is correct
  and is now precisely evidenced by the seven tests that actually catch it.
- Added one production test-seam accessor (`Pool::writerCleanupDutiesPendingForTest`) not explicitly
  named in the plan's Files list, needed to assert Step 1's pin directly rather than only via a
  build-floor proxy.
