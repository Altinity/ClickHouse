# T2: Task 6b remainder — publication-ordering coverage

## Status: DONE, all green

New file: `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp`, suite
`CasRefSnapshotPublishOrdering`, 5 tests (4 requested + 1 sensitivity-check test). No production code
changed.

## Deviation from the brief, disclosed up front

The plan's sketch for test 3 (`PoisonedRefusesPublicationAndTriggersReRecovery`) asked to assert "zero
body PUTs and zero `_ckpt` writes" when a publish is requested against a poisoned lane. I flagged this to
the dispatcher before implementing (see message log) and, receiving no correction, proceeded on the
documented alternative: `tryPublishSnapshotAndAdvanceCheckpointOnce` calls `ensureRefTableRecovered`
unconditionally, and that function re-walks the durable stream whenever the lane is `NeedsRecovery`
regardless of `recovered`. Every reachable way to reach `NeedsRecovery` (there is no `*ForTest` seam that
installs it directly) leaves a real durable gap between the log and the checkpoint — that is what
`NeedsRecovery` means — so the SAME call's re-recovery closes that gap with its own `_ckpt` CAS, and
because the test's table had never published a snapshot at all, the same call then finds a real,
uncovered candidate and successfully publishes one. Empirically confirmed (see the sensitivity-check
mutation run below): the call returns `true`, not `false`. The test as written instead pins what IS true:
recovery's own checkpoint catch-up CAS lands strictly before any snapshot-publish effect, and the
snapshot publisher's own body PUT lands strictly before its own checkpoint CAS — i.e. recovery-then-publish
is the only order this call can ever produce, never publish-around-a-still-poisoned-lane. The test name is
kept as the brief specified; the doc comment above it explains the finding in full.

## Per-test record

1. **`SnapshotBodyIsDurableBeforeCheckpointAdvances`** — drives one successful publish via
   `tryPublishSnapshotAndAdvanceCheckpointOnce` after a birth transaction, using a custom
   `OrderedFaultBackend` (extends `CountingBackend`, adds an ordered PUT/CAS journal). Positive control:
   the snapshot-publish attempt itself touches the body key and the `_ckpt` key exactly once (measured as
   a delta across the call, since the birth transaction's own `_ckpt` CAS writes precede it and must not
   be mistaken for the publisher's). Asserts `body_index < ckpt_index`.
   - First run: RED (caught two real setup bugs before the fix — see run log). After the fix: GREEN on
     the first genuine run — this pins pre-existing behavior. `build/t2_run.log`.
   - Sensitivity: a companion always-green test
     (`SensitivityCheckOrderingComparisonDiscriminates`) asserts the swapped (wrong) direction fails.
     Additionally, load-bearing mutation demonstration performed after implementation; mutation reverted;
     patch and failing output preserved: flipped `EXPECT_LT` to `EXPECT_GT` directly in this test,
     rebuilt, and it failed (`build/t2_sensitivity_1.log`, line 226); reverted and reconfirmed green
     (`build/t2_run.log`).

2. **`AdoptionHappensLastAndOnlyAfterBothDurableEffects`** — arms a persistent `Conflict` on the `_ckpt`
   key for all 100 of `publishCkpt`'s attempt-bounded retries (`MAX_CKPT_CAS_ATTEMPTS = 100`, confirmed by
   reading `CasRefCkpt.cpp`), so the body PUT commits but the checkpoint never advances within that call.
   Asserts no in-memory adoption occurred, then disarms the fault and retries (the same retry unit),
   asserting adoption happens exactly once, after both effects are durable.
   - First run: RED — `putCount(snapshot_key)` was asserted to stay at `1` after the retry; the counter
     actually counts CALLS (2, since the retry issues its own `putIfAbsent` attempt that resolves via
     dedup), not "did a new object get created". Fixed the assertion and comment to state the real
     (equally sound) invariant. GREEN after the fix, `build/t2_run.log`.
   - Sensitivity: load-bearing mutation demonstration performed after implementation; mutation reverted;
     patch and failing output preserved: flipped the "adoption must NOT happen" `EXPECT_FALSE` to
     `EXPECT_TRUE`; it failed (`build/t2_sensitivity_1.log`, line 284); reverted, reconfirmed green.

3. **`PoisonedRefusesPublicationAndTriggersReRecovery`** — see the deviation note above for the full
   finding. Reaches `NeedsRecovery` the same way `gtest_cas_ref_writer.cpp`'s
   `RefWriterAppendLane.CheckpointConflictAfterLogCommitRequiresRecoveryWithoutInstall` does (persistent
   conflict on a mutation's own commit-time `_ckpt` CAS). Asserts: (a) the call succeeds (recovers, then
   publishes); (b) the lane is `Ready` afterward and `recoveryInstallCountForTest` advanced (re-recovery
   is observable, not a silent skip); (c) recovery's own checkpoint catch-up CAS index precedes the new
   snapshot's body-PUT index, which precedes the publisher's own checkpoint-CAS index.
   - No genuine red-first run exists for this predicate — the plan itself anticipated this ("genuine
     red-first is likely impossible... a coverage pin with a sensitivity check"). It IS a coverage pin
     with no prior test naming this ordering triple.
   - Sensitivity: load-bearing mutation demonstration performed after implementation; mutation reverted;
     patch and failing output preserved: changed the expected post-recovery lane state from `Ready` to
     `NeedsRecovery`; it failed (`build/t2_sensitivity_1.log`, line 356); reverted, reconfirmed green.

4. **`PublishBackoffDecisionsAreCharacterized`** — characterizes `admitSnapshotPublishUnderStateLock`,
   `advancePublishBackoff`, `resetPublishBackoff` through the public dispatch surface, using
   `PoolConfig::boot_ms_fn` (a real, existing clock seam — `gtest_cas_ref_writer.cpp`'s
   `C4BackoffDefersThenRetriesAndPublishes` already relies on it) rather than falling back to the
   attempt-count-only characterization the plan anticipated as the likely limitation. Pins the exact
   backoff schedule (1000ms initial, doubling to 2000ms, doubling again to the 4000ms cap) against literal
   clock offsets, using `ProfileEvents::CasRefSnapshotPublishDispatched` as the dispatch-count observable,
   and confirms `resetPublishBackoff` clears the cooldown (the next over-threshold trigger at the *same*
   clock reading as a successful publish dispatches immediately).
   - First run: RED, twice, both fixable-setup bugs rather than a wrong understanding of the primitives:
     (a) faulting the `_ckpt` key (as tests 2/3 do) also poisons the append-commit's OWN checkpoint write,
     which is a DIFFERENT ckpt writer sharing the same key — this drove the append lane into
     `NeedsRecovery` instead of exercising the snapshot-publish backoff, so the fault was switched to the
     snapshot BODY put (`_snap/` substring) with a single-attempt `CasRequestBudget` (copied from
     `C4BackoffDefersThenRetriesAndPublishes`) so a fault resolves to a definite failure with no internal
     retry and no wall-clock wait; (b) `newestPublishedSnapshotIdForTest`'s baseline was asserted
     `has_value() == false`, but the birth transaction's own auto-dispatched publish (threshold 0) already
     has one — fixed to compare against the captured baseline rather than presence. GREEN after both
     fixes, `build/t2_run.log`.
   - Sensitivity: load-bearing mutation demonstration performed after implementation; mutation reverted;
     patch and failing output preserved: changed the "must not re-dispatch within the backoff window"
     expectation from `d1` to `d1 + 1`; it failed (`build/t2_sensitivity_1.log`, line 445); reverted,
     reconfirmed green.

   NOTE (`CasRequestControllerBackoff` distinction, per the brief): this test characterizes ONLY the
   per-table snapshot-publish dispatch backoff (`RefTableRuntime::publish_backoff_ms`/
   `publish_backoff_until_ms`, driven by `admitSnapshotPublishUnderStateLock`); the request controller's
   own per-attempt retry backoff is a separate mechanism and was not touched or characterized here.

## Discovered limitation

`admitSnapshotPublishUnderStateLock`, `advancePublishBackoff`, `resetPublishBackoff` are `private` to
`CasRefLedger`, so all characterization goes through the public dispatch surface
(`appendRefOps`/`resolveRef` → `maybeScheduleSnapshotPublish`) rather than calling them directly — this
means the backoff test's assertions are about OBSERVABLE dispatch behavior (counts, published-snapshot
identity), not direct return values of the private methods.

## Runs (both green)

- Release: `build/t2_run.log` — `[  PASSED  ] 5 tests.`
- ASan: `build_asan/t2_run.log` — `[  PASSED  ] 5 tests.`
- CA gate suite generator: `build/t2_suites.log` — `wrote 278 suites ... (21 excluded, 0 unclaimed)`;
  `build/cas_suites.txt` contains `CasRefSnapshotPublishOrdering`.

## Build logs

`build/t2_build.log` (first attempt, failed on an unused-member-function `-Werror`), `build/t2_build2.log`
through `build/t2_build4.log` (iterative fixes), `build/t2_build_final.log` (final, after mutation
revert), `build_asan/t2_build.log`.

## Commit

One commit: new test file only (report and this file added via `git add -f` where needed — the report
lives under `.superpowers/sdd/`, already tracked as a project directory).
