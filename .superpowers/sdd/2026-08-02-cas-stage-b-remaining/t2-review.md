# T2 review — `gtest_cas_ref_snapshot_publish_ordering.cpp`

Reviewed as one slice: `404b6ecbe3a` + `ecc638f8861`.

**Verdict: APPROVE-WITH-NONBLOCKING.** No defect found in the test logic that blocks the slice; four
nonblocking follow-ups (F1–F4) and three PROSE items (P1–P3).

Everything below was checked against the code, not the report. Verified production symbols:
`CasRefLedger::tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime`, `admitSnapshotPublishUnderStateLock`,
`advancePublishBackoff`, `resetPublishBackoff`, `dispatchSnapshotPublisher`, `settleSnapshotPublish`,
`waitForSnapshotPublishSettleForTest`, `publishCkpt` (`CasRefCkpt.cpp`), `throwCasWriteRetryLater`
(`CasRequestControl.cpp`), `Layout::refSnapshotKey`/`refCkptKey`.

## R0 — RESOLVED by `ecc638f8861`: evidence and committed source now agree

An earlier pass of this review flagged that `404b6ecbe3a` (03:45:18) still held the **pre-ruling** test 3
(`PoisonedRefusesPublicationAndTriggersReRecovery`, deviation framing, no arm (b)) while the green logs and
both binaries (03:49:34 / 03:50:10) attested an uncommitted working tree. `ecc638f8861` (03:51:20) commits
exactly that tree: `git diff ecc638f8861 -- <test file>` is empty and `git status` is clean for the test
file and the report. Both `build/t2_run.log` and `build_asan/t2_run.log` name the renamed test, so the
release and ASan greens are now evidence about the committed source. `build/t2_sensitivity_1.log` (03:41)
names the old test and remains valid evidence for tests 1/2/4; `build/t2_sensitivity_3.log` (03:49) is the
new test-3 evidence — see the arm-(b) verification in item 3 below.

## Answers to the dispatch questions

**1. Test 1 — real order, not call counts: YES.** `OrderedFaultBackend::record` appends to an ordered
journal from inside the overridden `putIfAbsent`/`casPut`, and the test compares journal *indices* scoped
from `journalSize()` captured after the birth transaction. Production order is
`ref_request_controller->putIfAbsentControlled(refSnapshotKey(...))` and only then
`publishCkptContribution(...)`; a swap makes `ckpt_index < body_index` and `EXPECT_LT` fails. The window
scoping is load-bearing and correct (the birth commit's own `_ckpt` CAS precedes `offset`), and the
positive control (`putCount`/`casPutCount` delta each exactly 1) rules out the "busy log picked the wrong
entry" artifact. `firstIndexFrom` on the ckpt key is safe here because the lane is `Ready`, so the only
ckpt CAS in the window is the publisher's.

**2. Test 2 — the negative is asserted, and the budget is bounded: YES.**
`EXPECT_FALSE(newestPublishedSnapshotIdForTest(ns).has_value())` is the negative, and it is not vacuous:
`EXPECT_EQ(putCount(snapshot_key), 1u)` first proves the body PUT did land, so the absence of adoption is
about a real half-completed publish. Production matches: on `!ckpt_advanced` it increments
`CasRefCkptNotAdvanced`, calls `advancePublishBackoff` and returns `false` *before* the adoption block, and
`resetPublishBackoff` + adoption sit together under one `state_mutex` hold after the CAS.
Resource/wall-clock: `publishCkpt`'s loop is `for (attempt < MAX_CKPT_CAS_ATTEMPTS=100)` with **no sleep
anywhere in `CasRefCkpt.cpp`** (grep clean) — each attempt is one in-memory `readCkpt` + one `casPut`. It
is additionally deadline-bounded, and if the deadline fires early the loop `break`s into the same
`throwCasWriteRetryLater` (`NETWORK_ERROR`), so a slow machine changes the attempt count but not the
outcome. Measured 42 ms release / 350 ms ASan. Adoption is not journal-observable (it is an in-memory
field), so "after both effects" rests on the negative arm — that is the strongest available observation
and it is the right one.

**3. Test 3 vs the ruling — all three arms covered, verified against the code and the sensitivity log.**
- (a) covered, and strengthened by the rework: the loop
  `for (snap_put_index : snap_put_indices) EXPECT_GT(snap_put_index, recovery_catchup_index)` is the
  "no `_snap` PUT at any index before recovery" form the ruling asks for, and
  `EXPECT_LT(snap_put_indices.front(), publisher_ckpt_index)` keeps the publisher's own `_ckpt` after its
  body. The discriminator is sound: if publication ran before recovery, `ckpt_cas_indices.front()` would
  be the publisher's own CAS, which sits *after* the body PUT, and the `EXPECT_GT` fails.
- (b) **covered**, in the strong form the ruling asked for:
  `EXPECT_EQ(newestPublishedSnapshotIdForTest(ns), std::make_optional(missing_durable_txn))` plus
  `EXPECT_FALSE(resolveRef(ns, "ref_1").has_value())`, on top of
  `ASSERT_FALSE(snap_put_indices.empty())` where `snap_put_indices` is scoped to
  `refSnapshotKey(life, missing_durable_txn)`. This is genuinely a frontier assertion, not a
  "some snapshot appeared" one: the publisher keys the body at `refSnapshotKey(rt->life, candidate_x)` with
  `candidate_x = rt->state.getGreatestApplied()`, so a snapshot published from the stale view would be
  keyed `{epoch,1}` and both the PUT lookup and the id equality would fail. `resolveRef` returns
  `std::optional<Resolved>`, so `has_value()` is the right probe, and its `false` can only come from the
  recovered view (the stale view still had `ref_1` committed).
  `build/t2_sensitivity_3.log` confirms both new assertions are load-bearing: with the ordering comparison
  and the `resolveRef` expectation inverted, the run fails at both sites, and the ordering failure prints
  `actual: 118 vs 117` — recovery's catch-up CAS immediately followed by the publisher's body PUT, i.e. the
  journal really does observe two adjacent, correctly-ordered effects rather than a wide vacuous gap.
  One residual: the snapshot **body is never decoded**, so "the removal is in the published snapshot" rests
  on the frontier id plus a read of the same recovered cache the body was built from, not on the bytes.
  That is a reasonable stopping point (decoding would need a listing/decode helper) and is recorded rather
  than raised. The `newestPublishedSnapshotIdForTest` equality itself was not among the mutated
  assertions, so its sensitivity is argued (equality against a computed id) rather than demonstrated.
- (c) covered: `laneStateForTest(ns) == Ready` and `EXPECT_GT(recoveryInstallCountForTest(), before)`.
- Doc comment: the reworked version states recover-then-proceed as the correct production reality and no
  longer frames it as a deviation. It does still reference "this task's plan" — see P1.
- Worth recording for later tasks: the plan's "refuses publication" arm *does* exist in production
  (`blocked_lane` → `LOG_WARNING` → `return false` when `lane_state != Ready`), but it is unreachable for
  `NeedsRecovery` because `ensureRefTableRecovered` runs first; it is reachable only for `Writing`/`Wedged`
  lanes. Nothing in this suite covers that arm, and nothing in the ruling asked it to.

**4. Test 4 — schedule pinned only partly; the `resetPublishBackoff` arm is vacuous.** See F1 and F2. The
clock seam is real and used exactly as the neighbour uses it. No wall-clock dependence: `fake_now` is the
only time source (`config.boot_ms_fn`), `waitForSnapshotPublishSettleForTest` waits on
`pending_snapshot_publishes == 0` (a cv, not a sleep), and the single-in-flight gate plus the frozen clock
make each step exactly one dispatch. `ProfileEvents` are read as deltas against `d1`, which is the
established idiom — `gtest_cas_ref_writer.cpp` does the same at eight sites. Intra-test pollution is
excluded: after a *successful* publish the captured tail is subtracted so `settleSnapshotPublish`'s
re-admission sees `tail == 0`, and after a *failed* one the freshly-armed backoff refuses. Cross-test
pollution from a detached publisher of an earlier test is not structurally impossible (`pin_owner` keeps
the pool alive and settle can re-dispatch), but that is a property of the pre-existing idiom, not of this
slice; both lanes are green.

**5. The 5th test — not a legitimate sensitivity check.** See F3.

**6. Sanitizer sweep / comment policy / logs.**
`grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"` on the file: **zero hits**. The single
throw expectation is `expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(...); })`, and
the actual throw site `throwCasWriteRetryLater` raises `ErrorCodes::NETWORK_ERROR` — code matches site, no
death-test split needed. `build/t2_sensitivity_1.log` shows 4 FAILED / 1 PASSED with the four failures on
the four real tests and the always-on companion still passing; the release and ASan logs each show
`[  PASSED  ] 5 tests.` ASan is a genuinely separate binary (`build_asan/src/unit_tests_dbms`, own
timestamp), so the ASan green is real. Comment-policy violations: P1.

## Positive contract property: no reader can observe an un-recovered cache

Verified, and it holds more broadly than reported. **Every** caller of
`CasRefLedger::acquireReadableRefTableRuntime` calls `ensureRefTableRecovered(ns, *rt)` immediately after
the null check and before taking `state_mutex`: `resolveRef`, `listRefs`, `hasAnyRefWithPrefix`,
`namespaceFilesLifeIfReadable` — four sites, no exceptions. The only bypass is `rt == nullptr` (a namespace
the catalog does not name), which returns absent rather than stale data, so it exposes nothing. The
`allow_stale` parameter of `resolveRef` is inert (`bool /*allow_stale*/`, kept only so existing callers
compile), so no caller can even request a stale read. Recovery therefore precedes every state read on the
public read surface, which is exactly the property that makes test 3's `resolveRef` probe meaningful, and it
independently corroborates the T1a site-1/site-2 KEEP reasoning: the unconditional-recovery preamble is
load-bearing at those sites, not redundant.

## Findings

### F1 — TEST (nonblocking): test 4's `resetPublishBackoff` assertion holds even if `resetPublishBackoff` is a no-op

`admitSnapshotPublishUnderStateLock` admits on `now >= rt.publish_backoff_until_ms`. The successful 4th
attempt necessarily happens at a clock reading that already satisfies that, i.e. at
`fake_now == publish_backoff_until_ms` (the test advances exactly `+4000` onto the armed deadline). The
final trigger then runs at the *same* `fake_now`, so `now >= until` is still true with the stale deadline
in place. Deleting the body of `resetPublishBackoff` leaves the test green — the assertion message ("must
have cleared the cooldown") claims more than the test can detect. `publish_backoff_ms = 0` is also
unobserved, because no failure follows the success.

The only observable consequence of the reset is that the *next* failure re-arms at the **initial**
interval instead of continuing from the cap. Suggested follow-up: after the successful publish, arm one
more `_snap/` PUT failure, let a trigger dispatch and fail, then assert refusal short of 1000 ms and
admission at 1000 ms. That observes both fields and, as a bonus, discriminates the cap from the initial.

### F2 — TEST (nonblocking): the 4000 ms cap is not pinned from below

The schedule is pinned as: interval 1 ∈ (0, 1000], interval 2 ∈ (1000, 2000], interval 3 ∈ (0, 4000].
There is no refusal check between the third failure and the `fake_now += 4000` crossing, so any third
interval ≤ 4000 passes — a regression that stopped doubling at 2000, or read `initial` where it meant
`max`, would not fail this test. Suggested follow-up: insert `fake_now += 2000; resolveRef;
EXPECT_EQ(dispatchCount(), d1 + 2);` before the final `+2000` crossing.

### F3 — TEST (nonblocking) + PROSE (FALSE): `SensitivityCheckOrderingComparisonDiscriminates` proves nothing

`EXPECT_FALSE(*body_index > *ckpt_index)` is the same predicate as test 1's
`EXPECT_LT(*body_index, *ckpt_index)`, only weaker (it also admits equality, which distinct journal
entries can never produce). It re-runs the identical scenario and would fail in exactly the same
circumstances as test 1 — so it cannot be evidence that test 1 is non-vacuous, which is precisely what its
own message claims ("this is what proves the assertion is load-bearing rather than tautological"). That
claim is FALSE. Not a tautology (it does exercise the real path), but a duplicate dressed as a control,
and it borrows the mandatory mutation-demonstration wording for something that is not a mutation.
Recommend deleting it — the real evidence is the recorded mutation run in `build/t2_sensitivity_1.log` —
or replacing it with a check that feeds `firstIndexFrom` a synthetically reversed journal, which would
actually test the observation mechanism.

### F4 — TEST (nonblocking, coverage): `settleSnapshotPublish` is a named subject but is not characterized

The plan lists `settleSnapshotPublish` among T2's test subjects. Tests 2 and 3 call
`tryPublishSnapshotAndAdvanceCheckpointOnce` directly and so bypass dispatch/settle entirely; test 4
depends on settle only implicitly (through `waitForSnapshotPublishSettleForTest`). Its load-bearing
property — decrement and re-admit under one `state_mutex` hold, so `pending_snapshot_publishes` never
transiently reaches 0 and the settle wait cannot observe a false "settled", and so chunks 2..N of a
chunked tenure are not suppressed — has no test naming it here. Not a defect in what was written; recorded
as the residual gap.

### P1 — PROSE (policy violation): plan/task references in comments

Batch into `docs/superpowers/cas/deferred-docs-fixes.md`. Four sites in the test file cite internal
documents, which the Global Constraints forbid outright:
- the file header: `Task 6b remainder (Stage B, {#t2}) ... the coverage that Task 6b's rename left undone`
  — a plan anchor plus a task number;
- test 3's comment: `Poisoned is this task's plan's name ...`, `Recorded here as the vocabulary correction
  for later tasks`, `This test is the plan's PoisonedRefusesPublicationAndTriggersReRecovery`;
- test 4's comment: `unlike the plan's anticipated fallback`.

The durable reasons are all fine and should stay (that `NeedsRecovery` is the state formerly called
`Poisoned`; that a controlled clock exists so the schedule can be pinned by literal offsets). Only the
citations need to go. Spec-invariant names such as `INV-4` are established tree-wide vocabulary and are
not affected.

### P2 — PROSE (IMPRECISE): "the exact backoff schedule … against literal clock offsets"

Both the test-4 doc comment and the report state that the test pins 1000 → 2000 → 4000 exactly. Per F2 it
pins half-open bounds, and the third interval has no lower bound at all. Reword to "brackets each interval
between a refusal and an admission" — or add the assertion from F2 and keep the wording.

### P3 — PROSE (IMPRECISE): report §2's dedup narrative

`putCount(snapshot_key) == 2` after the retry is described as the retry's PUT "resolved via dedup against
identical, already-durable bytes rather than writing a second object". `OrderedFaultBackend` delegates to
`CountingBackend::putIfAbsent`, so what the test actually observes is that the counter counts *calls*; that
the second call resolved rather than overwrote is an inference from the in-memory backend's semantics, not
something this test observes. The invariant asserted is sound; only the confidence of the phrasing is off.
