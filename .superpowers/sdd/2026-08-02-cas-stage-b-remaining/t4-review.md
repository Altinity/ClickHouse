# T4 review — commits `b17e4d97485` + `4329577bf37` (Task 8 closure: duty queue + orphan nomination)

**Verdict: APPROVE-WITH-NONBLOCKING.**

`4329577bf37` is **docs-only** — it edits `t4-report.md` and nothing else (`git show --stat`: 1 file,
+9/−5). The source tree is byte-identical across the two commits, so every code conclusion below,
and the independent ASan gate in §0, applies to the slice as a whole unchanged.

No product defect found. The Q-1 decision is implemented as stated, C-1 is genuinely discharged, and
the corrected ASan gate is clean at this tree. Two findings need placement: one TEST finding (Step 4's
real-round accounting has no detection power), and one CODE finding against the **gate tooling** (the
per-suite runner cannot run parameterized suites at all, and its fixed 60 s budget is below two
suites' ASan runtime). The rest are PROSE.

---

## 0. Corrected ASan gate — the missing evidence, now supplied

### 0a. Provenance of the implementor's `build/t4_asan_gate3.log` — **REJECTED; ran my own instead**

Content checks pass: the filter header inside the log names the full 296-suite set (death-test suites
and both TEST_P instantiation spellings included), the run reaches
`[==========] 1990 tests from 296 test suites ran. … [  PASSED  ] 1990 tests.` — so no abort truncated
it despite being a single invocation — `YOU HAVE 2 DISABLED TESTS` matches my own count, and the
marker line `T4_ASAN_GATE3_DONE=0` is present.

**The binary attribution does not hold.** The log's mtime is `06:35:14.283` and gtest reports
`250649 ms`, so the run started at ≈`06:31:03`. My rebuild relinked
`build_asan/src/unit_tests_dbms` at `06:32:35.738` — **inside that window**. A running process keeps
the inode it exec'd, so `t4_asan_gate3.log` attests to the *pre-rebuild* binary (last linked
`05:53`), which predates the `06:14` mtimes on the two edited test files. This is the same
concurrent-build hazard the T4 report itself documents under "Notable debugging", recurring one
level up.

So I did not accept the log; the §0 numbers below are from my own run, started `06:32:36` — one
second after the link completed — per-suite, against a clean `src/` at this commit. They agree with
`t4_asan_gate3.log` exactly (296 suites, 1990 tests, 2 disabled, 0 aborts, 0 sanitizer reports),
which is good corroboration but is not what makes them valid. **Do not cite
`build/t4_asan_gate3.log` as standalone evidence**; cite the run below.

### 0b. The gate result

Regenerated from the ASan build and run per-suite at this tree (`b17e4d97485`, `src/` clean):

```
utils/cas-gate/generate_cas_suites.sh build_asan
  -> wrote 296 suites to build_asan/cas_suites.txt (3 excluded, 0 unclaimed)
utils/cas-gate/run_cas_gate_per_suite.sh build_asan
  -> TOTALS: pass=291 fail=5 abort=0
```

The five FAILs are **all runner artifacts, not test failures**. After correcting both defects and
re-running the affected suites by hand:

| Suite | Runner verdict | Cause | Corrected |
|---|---|---|---|
| `InMemory/CasBackendContract` | FAIL (exit 1) | log redirect to `suite_logs/InMemory/…` — dir absent, **suite never executed** | PASS, 16 tests |
| `Local/CasBackendContract` | FAIL (exit 1) | same | PASS, 16 tests |
| `WinnerShape/CasGcCompletedRemovalFenceRace` | FAIL (exit 1) | same | PASS, 2 tests |
| `CasGcStopStart` | FAIL (exit 124) | 60 s runner budget < 69.1 s ASan runtime | PASS, 10 tests (69.5 s wall) |
| `RefWriterStalePrecommitSweep` | FAIL (exit 124) | 60 s runner budget < 60.1 s ASan runtime | PASS, 4 tests (60.7 s wall) |

**Corrected ASan gate result: 296/296 suites, 1990/1990 tests passed, 0 aborts, 0 sanitizer
reports.** The 1990 figure is cross-checked against `--gtest_list_tests` under the generated filter
(1992 listed − 2 `DISABLED_` = 1990) and equals the sum of the per-suite logs, so nothing was skipped
silently. Death-test suites are present in this list (e.g. `CasRefCatalogDeathTest`), which the
implementor's 278-suite release-list run did not cover — that gap is now closed and it is clean.

The 296-vs-278 delta is exactly the 18 `*DeathTest` suites and nothing else (`diff` of the two
generated lists: 18 added, 0 removed, 0 changed), so `4329577bf37`'s "+18, exactly the 18 `*DeathTest`
suites" is precisely correct.

**Coverage observation, not a finding.** `KNOWN_COMPILE_GUARDED` in `generate_cas_suites.sh` holds
**19** names, but only 18 appear in the ASan binary. The nineteenth,
`CasRefInstallSafetyDeathTest.DenyGuardStopsAnAllocation`
(`src/Disks/tests/gtest_cas_ref_install_safety.cpp`), is in **neither** build — it is gated on
`MEMORY_TRACKER_DEBUG_CHECKS`, which requires `!NDEBUG`, and sanitizer builds define `NDEBUG`. The
test's own comment documents this deliberately, including the earlier bug where it was gated on
`DEBUG_OR_SANITIZER_BUILD` and "failed to die" on all three sanitizer lanes. So the exclusion is
correct and the guard list is behaving as designed — but the campaign should know that this one test
runs in **no** gate lane and needs a plain-debug build to exercise at all.

Sanitizer-report sweep across all 296 suite logs plus the two re-runs
(`ERROR: AddressSanitizer|LeakSanitizer: detected|SEGV on unknown|runtime error:`): zero hits.

Delta accounting: 1990 = 1986 pre-T4 + T4's 4 new tests. The 1982/295 figure recorded at the T1-lane
close moved because the T2 lane (`166d8960edf` and predecessors) landed in between — not a T4
concern, but it makes the report's own delta line wrong (see PROSE-1).

### CODE-1 (gate tooling, not T4's diff) — `run_cas_gate_per_suite.sh` cannot run TEST_P suites

`generate_cas_suites.sh` deliberately emits parameterized suites under their instantiation spelling
`<Inst>/<Suite>` ("so the emitted filter names what is actually runnable"). The runner then writes to
`"$LOG_DIR/${suite}.log"` and only ever `mkdir -p "$LOG_DIR"` — for a slash-bearing name the redirect
fails, the shell reports exit 1, **and the binary is never invoked**. Three suites at this tree are
affected. The failure mode is the dangerous direction: it reports FAIL, so it is not silent, but the
suite contributes no evidence either way and the natural reading of a red line is "the test broke".
Fix: `mkdir -p "$(dirname "$logf")"` before the redirect. Separately, the fixed `timeout 60` is below
two suites' honest ASan runtime; a per-suite budget that a passing suite exceeds turns the gate into
a flake source. Both belong to whoever owns `utils/cas-gate/` — placement: T8's battery step, which
runs this script as its gate.

---

## 1. Q-1 acceptance mapping — verified against code

### (1a) `DutySurvivesSettlementFailureForRetry` — **the injected failure does hit the settlement
append, and the duty is genuinely retained and retried. Verified, not accepted.**

Ordering in `Pool::drainWriterCleanupDuties` is exactly as the Q-1 decision requires: the
`ref_ledger.appendRefOps` carrying the exact `OwnerTransition` removal (or the conclusive-absence
`return {}`), then `retireBuildSeq(duty->build_seq)`, then `pop_front()`. The `catch (...)` arm only
clears `draining`, notifies, and rethrows — it never pops and never retires.

The fault reaches that append and no earlier step, for three independent reasons:

- `ChunkFaultBackend` faults **only** in `putIfAbsent` on a `fault_substr` match; `Mode::Unresolved`
  throws *before* delegating to `CountingBackend`, so nothing lands. The armed substring is the
  namespace's `_log/` stream prefix, and everything before the arm (`casAdmitRecoverableEntry`,
  `publishEmptyRef`, the `durable` precommit's own append) has already completed durably.
- `mutateRefsAfterWriterCleanup` drains **before** the mutation, so the settlement's `_log/` PUT is
  the first one after the arm and `fault_count = 1` consumes it there.
- Independently of ordering: the test's `EXPECT_TRUE(writerCleanupDutiesPendingForTest())` after the
  throw can only hold if the drain did not complete, i.e. the throw was inside the drain.

The test is **not vacuous** on the sticky-failure bit. `writerCleanupDutiesPending` also returns true
when `writer_cleanup_queue_failed` is set, and that bit is never cleared — so the closing
`EXPECT_FALSE(writerCleanupDutiesPendingForTest())` after the successful retry proves the bit was
never set and every earlier `true` was the real queue. `EXPECT_EQ(minActive(), durable_seq)` after the
throw independently proves `retireBuildSeq` did not run, and
`EXPECT_TRUE(resolveRef(ns, "target").has_value())` proves the blocked mutation aborted with the
settlement rather than proceeding past it. Retried to completion: the second `dropRef` succeeds, the
queue empties, and `minActive() == peekNextBuildSeq()`.

### (1b) `RejectedAttemptBodyIsEventuallyNominatedAndSwept` — **rejection is genuine; the body travels
the real round path; the watermark trap is avoided.**

- *Genuine rejection, provable absence of durable publication.* `Mode::Unresolved` throws without
  ever calling `CountingBackend::putIfAbsent` — this is absence by construction at the injection
  site, the strongest available form, not an inference. The test then pins
  `refLaneWedgedForTest(ns)` and `precommitState() == Uncertain`, and after the successor's
  `precommitAdd` the live-precommit set is exactly `{successor}`.
- *Real round path.* Deletion is driven by a real `Gc` over real `runRegularRoundReclaiming` rounds
  (bounded loop of 16, then `EXPECT_FALSE(head(...).exists)`) — the loop cannot pass vacuously,
  since a body that never disappears exits the loop still present and fails the assertion.
- *Watermark trap avoided, and the avoidance is load-bearing.* The namespace is rooted under the
  pool's own `server_root_id` (`"test/…"`, not this file's usual `"srv1/…"`), and the comment states
  the reason: `prefixEligible` derives its floor by walking the namespace's own prefix segments for
  a live mount lease, so a foreign-rooted namespace is permanently ineligible regardless of
  epoch/coverage. This is the right disclosure and it is in the code, not only in the report.

Minor observation (not a finding): the test asserts the body is gone, not that the *orphan sweep*
specifically removed it. Since nothing else in the round deletes a manifest, the inference holds.

### (1c) `SuppressedRoundNominatesNothing` — **suppression is production-realistic and the assertion
is against the round's own phase record, not a proxy.**

`runRegularRound()` with no third argument resolves to `UniversePolicy::kDefault`, and
`CasGc.h` pins `kDefault = StageA_Suppressed` — the single production call site (`CasGcScheduler`)
passes nothing, so this *is* the production universe. The report's cited gate is accurate: the
`planManifestCursorPage` call is guarded by
`if (!suppress_destructive && store->poolConfig().manifest_sweep_list_budget_keys > 0)`, so a
suppressed pass does not even LIST. The test reads `listed`/`deleted`/`suppressed` off the emitted
`orphan_sweep` `GcPhaseRecord` (`.at()` would throw on a missing key, so the metrics are proven
present) and additionally asserts the candidate survives. Non-vacuous: the same `makeReadyFixture`
under a reclaiming round *does* nominate and delete that candidate in
`RetiresExactManifestSourcesBeforeDelete`, so the contrast is real and the budget term of the gate is
not what is producing the zero.

### (1d) Step 4, accounting on the real round — **TEST-1: weaker than the plan meant, and neither
assertion can fail.**

The plan asked for `unmatched_removes == 0` **and "the B2 `applied` ordinal vector byte-stable across
the nominating round"**. What landed is `unmatched_removes == 0` and `txns_unapplied == 0`.

- `txns_unapplied == 0` is **tautological given the rest of the test**. `CasGc.cpp` computes
  `unapplied_txns` in `fold_reduce` and then throws `CORRUPTED_DATA` before the seal write whenever it
  is non-empty — the code comment says so outright ("0 on every committed round because a nonzero
  value throws a few lines below"). The test's preceding assertions (manifest deleted,
  `condemnedCount == 2`, the in-degree walk) already require a committed round, so this assertion
  cannot be false while they pass. It adds no detection power.
- `unmatched_removes == 0` is also non-falsifying **in this fixture**. `foldDeltasIntoGeneration`
  applies `source_retirements` in a loop separate from the delta loop and never increments
  `unmatched_removes` from it — that is precisely why the synthetic
  `SourceRetirementIsAccountingNeutral` supplies a deliberately *non-matching* retirement
  (`UInt128(0xA003)`) to make the point. The real-round fixture's retired edges are all present, so
  even a mutation re-routing retirements through the remove-delta path would match and stay at zero.
- The plan's actual ask is the one with teeth: the synthetic test's `applied` sentinel (`{0x5A}`
  preserved) detects the retirement touching `out_applied_by_txn_ordinal`. On the real round the
  equivalent state is `ledger.applied`, passed by address into the fold and not exposed afterwards —
  so the literal plan wording needs a new seam to satisfy.

Recommended disposition (needs a named owner, not a ledger line): either expose the round's
`ledger.applied` and assert the retirement set no bit, or amend the plan to say the real-round half is
a round-health check and the neutrality proof stays synthetic. Placement: T8's residual row, which
already carries per-task residuals.

---

## 2. C-1 — `cas_sweep_test_support.h`: **discharged, verified by grep, coverage preserved.**

- Zero production callers: `git grep -n "sweepManifestCursorPage" -- src/ | grep -v src/Disks/tests/`
  returns nothing (exit 1). Every one of the 19 references lives under `src/Disks/tests/`.
- The comment states the footgun property explicitly and correctly: plans via the production
  `planManifestCursorPage` then exact-token-deletes "with no source-edge retirement and no `gc/state`
  adoption … this shortcut recreates the accounting hole that path exists to close, so it must never
  be reached from a production translation unit." It names the reason, cites no plan or finding id,
  and is short. Compliant.
- Coverage preserved: all 15 call sites in the two test TUs moved 1:1 (`gtest_cas_orphan_manifest_sweep.cpp`
  ×11, `gtest_cas_sweep_deletion_premise.cpp` ×6 including the two-page resume pair). No test was
  deleted, weakened, or merged. The helper body is byte-identical to the removed function.

## 3. The mutation-demo deviation — **PROSE (plan), not a coverage gap. The implementor is right.**

Verified in code, both halves of the claim:

- The plan says to mutate `enqueueWriterCleanupDuty`'s "`Uncertain` branch". That function has **no
  such branch** — it unconditionally builds and queues the duty. The `Uncertain || Durable` test is in
  `PartWriteTxn::~PartWriteTxn`. The dtor is the only place the mutation can be applied, so the
  implementor's substitution is the faithful reading, not a deviation.
- `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant` uses a plain `InMemoryBackend`
  with no fault injection and a plain `precommitAdd`, so its abandoned precommit is `Durable` at dtor
  time. A `Durable`-only mutation genuinely does not touch it. The plan named the wrong test.
- Not a gap: the `Uncertain` arm is covered by tests that *assert* the state directly —
  `WedgeResolvedAsRejectDrainsTheDutyAsNoOp` pins
  `ASSERT_EQ(rejected->precommitState(), PrecommitState::Uncertain)` before the dtor runs, and the
  other six failing tests are the adopt/proven-absent/direct-API arms. The seven failures are the
  correct demonstration.

Grade: **PROSE, FALSE** on both counts (a function named that has no such branch; a test named that
is `Durable`-shaped). Batch into `deferred-docs-fixes.md`; the plan text should be corrected so the
next reader does not re-derive this.

## 4. `Pool::writerCleanupDutiesPendingForTest` — **clean.**

Public, `const`, one-line forward to the private `writerCleanupDutiesPending()`, no state change, no
new observable behaviour. Naming matches the file's established `…ForTest` convention
(`livePrecommitsForTest`, `refLaneWedgedForTest`, `lastEpochSealForTest`, `peekNextBuildSeq`). Three
callers, all in `gtest_cas_writer_duties.cpp`; zero production callers (grep-verified). The comment
gives the reason it exists rather than restating the code ("the direct signal that a settlement
failure retained its duty for retry, independent of any build-floor side effect that could have the
same shape for an unrelated reason") — which is exactly the argument that makes the test's assertion
stronger than the `minActive` proxy it replaces.

## 5. Sanitizer sweep, comment policy, S3 lane

**Sanitizer sweep** over the five touched test files
(`EXPECT_(ANY_)?THROW|ASSERT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR`): one hit,
`gtest_cas_orphan_manifest_sweep.cpp`'s `CursorPageRefusesAmbiguousCatalogLifeIndex` —
`EXPECT_THROW(..., DB::Exception)`. Checked against the actual throw site by running it: the raised
code is **246 `CORRUPTED_DATA`** ("catalog life_id … is shared by current namespaces … refusing a
decision from an ambiguous cut"), not `LOGICAL_ERROR`, so there is **no death-test-split abort risk**.
It is a loose fence of exactly the shape T-1 tightened, on a line this commit touched — but outside
T-1's named scope (the two tests in `gtest_cas_orphan_nomination.cpp`). **TEST-2, nonblocking**:
tighten it to `expectThrowsCode(CORRUPTED_DATA, …)` opportunistically.

**Comment policy**: no new comment cites a plan, spec section, BACKLOG entry, review round, finding
id or task number. The new comments give reasons (why this namespace root, why this fault mode, why
this seam) and are short. Two soft edges, not violations: "The model gate recorded both
wedge-resolution witnesses" (the model is a checked-in artifact, durable) and "rule (1) of the
sweep's deletion premise" (an enumeration that lives in prose elsewhere; the sentence still carries
its own meaning). No action.

**S3 lane** (`build/t4_s3.log`): **3/3 genuine passes** —
`test_content_addressed_s3::test_content_addressed_s3`,
`test_content_addressed_s3::test_mutations_and_patch_parts_survive_restart`,
`test_content_addressed_gc_s3::test_stage_a_gc_is_suppressed_and_says_so`, "3 passed in 68.74s". The
two `ERROR: command failed after 1/1 attempt(s)` lines are the harness's `docker info > /dev/null`
probe, before the run, not test failures. Note for calibration: T4 removes only a
production-unreferenced function and adds a header accessor, so production behaviour is unchanged and
this lane is a regression check rather than evidence about the new code.

## 6. Q-2 — **not claimed closed. Clean.**

The audit records Q-2 (orphan-sweep source edges retired and durable in the round's `gc/state` CAS
before the `TokenMismatch` throw fires) as an observation "for the closer's awareness only", and the
plan routes it to T8's residual row (item 6: record as accepted-by-design or turn into a comment at
the throw site). Neither the T4 report nor the commit message asserts it resolved. The report's only
mention of the ABA path is Step 3's error-code justification for `TokenAbaIsRetainedAndSurfaced`,
which is a different claim and is correct.

---

## Findings, labelled

| # | Label | Grade | Finding |
|---|---|---|---|
| CODE-1 | CODE | — | `run_cas_gate_per_suite.sh` never executes `<Inst>/<Suite>` suites (log redirect into a non-existent dir → reported FAIL, binary not invoked); fixed 60 s budget below two suites' honest ASan runtime. Not in T4's diff; place on T8's battery step. |
| TEST-1 | TEST | — | Step 4's real-round accounting has no detection power: `txns_unapplied == 0` is throw-gated (tautological given a committed round) and `unmatched_removes == 0` cannot be tripped by the retirement path in this fixture. The plan's `applied`-byte-stability ask was not implemented and needs a seam. |
| TEST-2 | TEST | — | `CursorPageRefusesAmbiguousCatalogLifeIndex` still uses `EXPECT_THROW(…, DB::Exception)` on a `CORRUPTED_DATA` site. No abort risk; tighten opportunistically. |
| PROSE-1 | PROSE | IMPRECISE | Report: "1985 … 4 more than the audit's cited pre-T4 baseline of 1980". 1980 + 4 = 1984. The 1985 count is itself correct (verified: 1987 listed − 2 `DISABLED_`); the delta line — whose whole job is to catch an unaccounted test — does not add up. |
| PROSE-2 | PROSE | FALSE | Report Step 4: the two real-round numbers are "the same two numbers `SourceRetirementIsAccountingNeutral` proves". That test proves `unmatched_removes` and `applied` byte-stability; it never observes `txns_unapplied`. |
| PROSE-3 | PROSE | FALSE | Plan Step 6 mutation 2 names `enqueueWriterCleanupDuty` (which has no `Uncertain` branch) and `PendingDutySkips…CrashRemnant` (which is `Durable`-shaped). Both wrong; the implementor's disclosure is accurate. |
| PROSE-4 | PROSE | ~~IMPRECISE~~ | **RESOLVED by `4329577bf37`.** Was: report Step 7 and `b17e4d97485`'s message state "Full ASan CA gate … 278 suites", a release-list run that excluded every compile-guarded death-test suite. The report now carries 296 suites / 1990 tests and keeps the 1957/278 run explicitly as non-gate evidence. `b17e4d97485`'s message is immutable and still carries the wrong figure; the report is now the correct record. |
| PROV-1 | — | — | `build/t4_asan_gate3.log` cannot be attributed to a binary: its run window spans my relink of `build_asan/src/unit_tests_dbms` (§0a). Its numbers are right — confirmed by an independent clean run — but the log is not evidence. Cite §0b instead. |

PROSE-1..3 → `docs/superpowers/cas/deferred-docs-fixes.md`, no fix round.
