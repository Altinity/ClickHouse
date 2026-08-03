# T6 review — destruction enablement arc (`laneg/t6-finish`)

Reviewed from `/home/mfilimonov/workspace/ClickHouse/master` via git objects; runs and log reads in
`/home/mfilimonov/workspace/ClickHouse/lane-g` (checked out at the arc tip, tracked tree clean), under
the shared `unit_tests.lock`. My logs: `lane-g/build/t6rev_build.log`,
`lane-g/build/t6rev_gate_release.log`, `lane-g/build/per_suite_results.txt`,
`lane-g/build/t6rev_killshot.log`.

Branch point `73755caa6e5`. Commits: `9d72d33d053` + `0f6c31caeaf` (picked draft), `a1686eb699a` +
`a37f5fc7f81` (verified waves), `2a058cbd08c` (janitor-drain adaptation), `2be5ba021ef` (report).

## Verdicts

| Group | Verdict |
|---|---|
| picks + waves (`9d72d33d053`, `0f6c31caeaf`, `a1686eb699a`, `a37f5fc7f81`) | **APPROVE** for integration into `cas-gc-rebuild`, with **TEST-1** as a required follow-up fix (it does not put the flip's safety in doubt — see below) |
| adaptation (`2a058cbd08c`) | **APPROVE** |
| report (`2be5ba021ef`) | **APPROVE**; two prose corrections to the batched register |

No reclaim of live data, no ungated destructive site, and no weakened load-bearing assertion was found.
Every restoration I sampled moved in the strict direction.

## 1. The flip itself — CLEAN

`kDefault = Authoritative` in `Gc/CasGc.h`, and the gate formula in `Gc::fold` is the plan's normative
three-term form character for character:

```
result.frontier_complete = universe_authoritative
    && result.frontier_namespaces > 0
    && result.frontier_proven == result.frontier_namespaces;
result.suppress_destructive = !report.anomalies.empty() || !carried_holds.empty() || frontier_incomplete;
```

The only edit inside the suppression-computation region is `policy == UniversePolicy::Authoritative`
(the renamed enumerator). No term was added, removed or reordered; the `per_round_cause` log split and
the new `frontier_complete` phase metric are reporting, not gating. Enum rename swept: zero
`AuthoritativeForTest` in `src/`, `tests/`, `utils/` at the tip (remaining hits are historical docs and
plans only). No production caller passes `StageA_Suppressed`.

## 2. O-2 — `planManifestCursorPage`'s authoritative arm: T4-reviewed behaviour, not an untested branch

Traced end to end. `catalog_recovery_authoritative` guards exactly one thing in
`planManifestCursorPage`: whether a **catalog-named** namespace's manifest debris may be recovered at
all. False retains it (`retained_no_coverage`, `errored_namespaces`); true proceeds to build the
protection view from the frozen catalog cut plus that life's `_ckpt` (`readCkpt` →
`activeManifestKeys`), still behind `prefixEligible`, the `Creating`-state refusal, the
protection-view try/catch, and the active-key check. Nomination is what it produces; deletion is
`Gc::fold`'s own retirement-then-delete path.

That arm is the one 27 gtests in `gtest_cas_orphan_manifest_sweep.cpp` and
`gtest_cas_sweep_deletion_premise.cpp` have always exercised — `cas_sweep_test_support.h` hard-codes
`/*catalog_recovery_authoritative=*/true` — and T4's review verified all 15 call sites moved 1:1 into
that helper. So production now reaches a branch tests have covered all along, not a fresh one.

Two notes, neither a defect. At the call site the argument is provably always `true` when reached
(`!suppress_destructive` ⇒ `frontier_complete` ⇒ `universe_authoritative`), so passing
`universe_authoritative` is equivalent to `true` there; keeping the term coupled to the gate is the
right shape anyway. And the newly-reachable call is now *observed*, not merely argued:
`CasGcRoundDefer.FoldAndDeferEachBuildExactlyOneCompletePostListWalkPlan` pins the catalog GET count at
3, the third being `planManifestCursorPage`'s own `CasRefCatalog::read`.

## 3. Dead-arm removals and the sum invariant — CORRECT, re-derived independently

Three removals, matching the t6a-review derivation exactly:

1. `if (!catalog_names_this_namespace)` (guarded by `const bool ... = true`) and with it
   `FrontierUnproven::NoCatalogEntry` / `no_catalog_entry`;
2. the frozen-tail exit and with it `AppendAboveFrozenTail` / `append_above_frozen_tail` and
   `WalkTarget::frozen_tail`;
3. the absent-record arm's `else frontier_proven = true`.

I re-derived (2) and (3) rather than trusting the derivation. The ceiling test
`if (*grounding->committed_through < *expected)` is the **first statement** of `while (expected)`;
`grounding` is never reassigned; `expected` is reassigned only on the crossing path, which `continue`s
back through the ceiling. So inside the body `*expected <= *grounding->committed_through` always holds:
(2)'s second conjunct is always false and (3)'s condition is always true. Both removals keep the
stricter surviving path — the unconditional `hold(GapBelowWitness)` — and the intake classification
counters are unchanged (held and empty-listing targets skipped classification before the change too).

Bucket decision is coherent. `FrontierDeficit::count` is called exactly once per namespace with
`frontier_proven == false`, so the buckets still sum to `frontier_namespaces - frontier_proven` by
construction. `unattributed == 0` still holds by construction: every non-proven exit names itself —
`CheckpointUnusable`, `CheckpointFrontierEmpty`, `CommittedBelowCursor`, and every `break` in the walk
is a `hold`/`fired` path that the effective-hold site relabels `Held` (I walked all eight breaks:
`WitnessDisappeared`, `GapBelowWitness` ×2, `UnconsumedSealCrossing` ×2, `BodyUndecodable`,
`ManifestBodyMissing`, and the `if (fired) break`). `probe_budget` and `fold_aborted` are added
directly, outside the per-namespace loop, and the header comment no longer claims an enumerator for
them. As a bonus the rewrite also fixes t6a's header-placement prose finding: the bucket contract now
sits on `struct FrontierDeficit`.

The removal of the listed-tail bound is a production behaviour *simplification* riding in the flip
commit and it is not in the plan's T6 file list — but since both arms are provably dead it is a no-op
on behaviour, and the termination guarantee is unbroken: the bound is
`_ckpt.committed_through`, snapshotted per namespace before the walk and never re-read.

## 4. The five suppressor arms — all present, all non-vacuous

`expectEveryDeleteFamilyInert` asserts blobs, `/cas/manifests/`, `/gc/gen/`, `/cas/ns/stream/` **and**
the total (so an unnamed family cannot slip through) — per family, not aggregate.

| Arm | Test | Non-vacuity |
|---|---|---|
| healthy | `AHealthyCatalogRoundOpensTheGateAndReclaims` | reads the verdict off the fold's own rows, then drains a real blob; M1 turns it red |
| (3a) explicit negative policy | `EveryInventoriedDestructiveSiteIsInertUnderSuppression` | ends with a control that drains the identical pool on the production path — drop the `universe_authoritative` term and `EXPECT_FALSE(frontier_complete)` fails |
| (1) anomaly | `AnUndecodableCheckpointAnomalySuppressesEveryDeleteFamily` | `anomaly_counts.front() > 0` proves the anomaly was recorded, not silently absorbed; the wave's fix lands the corruption on the catalog-resolved life so the round actually reads it |
| (2) carried hold | `ACarriedHoldSuppressesEveryDeleteFamily` | `ASSERT_GT(intake.at("tables_held"), 0)` on the detecting round before the five carrying rounds |
| (3b) empty-universe floor | `AGenuinelyEmptyUniverseRefusesTheFrontierDespiteZeroEqualsZero` | M2 (drop the floor) produces 9 real deletes; plus a one-namespace control that drains the same blob |
| (3c) budget | `AnExhaustedProbeBudgetSuppressesEveryDeleteFamily` | the wave's `frontier_unprobed_budget > 0`, read off `fold_ref_intake` rather than recomputed, makes the arm pin **its own** suppressor |

Arm 3c as drafted tested nothing (it suppressed on neither budget nor anything else); the finisher found
that, fixed the fixture by deleting the `_ckpt` by exact token, and added the term assertion. That is the
standard the rest of this review is measured against — and it is the standard TEST-1 below fails.

The three frontier_gate arms above also carry the *real* kill-shot property between them: a
catalog-named member that cannot prove its frontier keeps a genuinely-condemned blob alive pool-wide.

## 5. Findings

### TEST-1 (CODE/TEST, real — fix round) `CasListLiarEndToEnd.AnEntirelyHiddenNamespacesEdgeIsRefusedByTheProductionDefault` is inert w.r.t. the property it names

The arc rewrote this test's comment to claim the survival mechanism is catalog membership plus member
count: "`hidden` was born through the real writer path, so the CATALOG names it … `frontier_proven ==
frontier_namespaces` fails on member count alone". The fixture cannot produce that. `buildKillShot`
writes **no `_ckpt`** for either namespace (every other test in the file calls
`writeRecoverableCkptForRawFixture`; `writeRefLogTxnRaw` only `casAdmitEntry`s a catalog row). I ran the
test — `lane-g/build/t6rev_killshot.log`:

```
CAS GC ref intake: namespace 00/hidden@cas@ has no usable checkpoint -- ...
CAS GC ref intake: namespace 00/visible@cas@ has no usable checkpoint -- ...
CAS GC fold: destructive work SUPPRESSED this pass — 2 anomaly(ies), 0 held namespace(s),
  frontier INCOMPLETE (0 of 2 namespace(s) proven; unproven: checkpoint_unusable=2)
```

`visible` is unproven too, nothing folds, the blob's observable in-degree never reaches zero, and the
round is suppressed by the **anomaly** term. The test would pass unchanged if the universe term were
deleted from the formula outright, so it cannot distinguish the flip from its absence — a test that
passes because its fault never fires. Its pre-arc form was honest (it asserted the policy constant, and
that was true); the new claim is not.

Fix: ground both namespaces with `writeRecoverableCkptForRawFixture` so the visible `-1` really folds
and the blob is really condemned, then assert the survival together with the gate's own verdict, as the
frontier_gate twin now does. Deleting the arm would also be defensible — its property is covered by 3a,
3b and 3c — but leaving it as-is leaves a false claim in the file that owns the blocker's story.

Not a blocker for the flip: no safety property is left unproven, and coverage of pool-wide suppression
on an unproven catalog member survives in three non-vacuous arms.

### TEST-2 (CODE/TEST, minor) a test name that now states the opposite of its assertions

`CasGcFrontierGate.HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` now asserts
`frontier_complete == true` and `suppress_destructive == false` — the round is precisely **not** refused;
the blob survives on its own folded in-degree, which is the stronger outcome and the wave handled the
failure exactly right. But neither the test name nor the section header above it ("the round refuses
because the CATALOG names `hidden` and its own frontier is unproven") was updated, so both now
misdescribe the test. Rename to what it measures (the hidden edge is found by the exact-key probe and
saves the blob on a complete frontier) and fix the header's three-arm summary.

### PROSE findings (batch to `docs/superpowers/cas/deferred-docs-fixes.md`, no fix round)

- **IMPRECISE — report §9 provenance.** The stateless log's own banner reads
  `Connected to server 26.6.1.20000 … @ 30108b5ee642 laneg/t3-finish`, which contradicts "a server I
  started from `build/programs/clickhouse` (rebuilt at this tip)" on its face. The claim is **true** —
  I fingerprinted both binaries: `build/programs/clickhouse` and `build/src/unit_tests_dbms` each
  contain the arc's new string `"; the caller supplied no universe"` and neither contains the removed
  `"universe itself is not provable this stage"` or the removed no-catalog-entry anomaly text. The
  embedded version is stale cmake metadata from a configure on `laneg/t3-finish`. The report should
  have named that trap rather than leaving a reader with a log that reads as the wrong tree.
- **IMPRECISE — `test_cas_replicated_relink`'s restored guard.** The comment says "GC reclaiming
  them proves …" while the assertion is `assert reclaimed`, i.e. **at least one** of the abandoned
  blobs (the loop tries for all eight rounds, then accepts any). That matches the banner's
  "reclaim-at-least-one loop", so it is not a weakening — only the sentence overstates it.
- **Note — `gtest_cas_list_liar_end_to_end.cpp` header** still frames the kill shot as "the one shape
  arithmetic intake cannot save", while this arc's own evidence is that a *grounded* hidden namespace
  IS saved by folding its edge. Reword together with TEST-1.

### Watch item (not a finding)

`test_content_addressed_gc_s3` now asserts `suppressed_phases == 0` over the whole GC log. An
empty-universe round (the 3b floor) suppresses legitimately, so a round landing between mount and the
first namespace would fail this absolutely-quantified assertion. It did not fire: zero occurrences of
`destructive work SUPPRESSED` across all nine server logs of the six lanes — most likely because a
pool with nothing to do defers before publishing a gate verdict. Worth knowing before T8's soak reads
the same signal.

## 6. Closeout completeness — VERIFIED at the arc tip

| Check | Result |
|---|---|
| `git grep -in "task 7b" -- src/ tests/ utils/` | zero hits |
| `git grep -n "STAGE-A RETURN ITEM"` | zero non-historical (plan, design spec, midpoint audit, old Stage-B plan, this task's reports) |
| `git grep -n "STAGE-A CONTRACT"` | zero non-historical (same set plus the Stage-A RESULTS record) |
| `broken_tests.yaml` | all four entries (`05008`, `04290`, `05010`, `04295`) gone, with their block comment |
| the four stateless tests | byte-identical to pre-arc apart from removed banner comments |

Both restored gtest assertions are real and green in my gate: `gtest_ca_wiring.cpp`'s
`EXPECT_EQ(after.unreachable, 0u)` on the production-scheduler path, and `CasGcLog`'s
`EmitsStartFinishWithCounts` carrying all three things its marker owed (`saw_deleted`,
`deleting_finish_idx >= marking_finish_idx`, `total_deleted > 0`).

Seven banner rewrites read individually; each asserts the destruction-era contract and several are
strictly stronger than the pre-Stage-A original: `gc_s3` and `ref_snaplog` dropped
`entries_redeleted` from the deleted-count sum (a redelete is not a delete), `ref_snaplog` tightened
`preview_deletes=` to `preview_deletes=0`, `s28_s33_corner` restored `not monotone` as the whole
condition per its own banner, `assertions.py` restored zero tolerance for every leak class in both
`assert_no_leftovers` and `assert_reclaimable_drained`, and `test_leftovers.py` inverts the previously
permitted family into a failure while keeping the both-sides boundary tests.

## 7. Fix-wave sample (11 adaptations across 9 files) — no weakened load-bearing assertion

Sampled: `gtest_ca_wiring` (restore), `gtest_cas_gc_log` (restore), `gtest_cas_gc_round_defer`
(GET 2→3, third reader named), `gtest_cas_orphan_nomination` (suppression subject → explicit
`StageA_Suppressed`, correct side of the rule), `gtest_cas_gc_shard_incarnation` + `cas_test_helpers.h`
(rename only), `gtest_cas_gc_bounded_walk` (comments + rename only; 8/8 green, and
`ARawRecordBeyondTheCommittedFrontierCannotSuppressDestruction` already passed an explicit policy so
the flip cannot move it), `gtest_cas_gc_frontier_gate` ×3 (the wave), `gtest_cas_list_liar_end_to_end`
(the plan's one intentional Stage-B edit — the explicit-policy variant collapses into the production
case; the deleted sibling was equally inert on the same fixture, so no coverage is lost), plus the five
integration files and three soak-framework files above. Every adaptation matches its stated subject
rationale. The only defect is TEST-1, which is an *insufficiently strengthened* claim, not a weakened
assertion.

Sanitizer sweep over all nine touched test files: every `EXPECT_THROW`/`EXPECT_ANY_THROW`/
`expectThrowsCode` hit is pre-existing (the arc changed only the enum name inside one of them), and
`git diff … -- src/Disks/tests/ | grep '^[+-].*LOGICAL_ERROR'` is empty, so the death-test split does
not apply. `CasGcShardIncarnation.DuplicateLifeIdStopsDestructiveRoundAndRebuild`'s `DB::Exception`
expectation cannot be a `LOGICAL_ERROR` abort: that suite ran clean in the ASan gate, which aborts on
logical errors.

## 8. Gates and evidence I ran or verified myself

- **Release CA gate, my own run at the arc tip** (`ninja` reported zero recompiles, link only, so the
  binary is the tip's objects): `278 suites PASS, 0 fail, 0 abort`, **1989 tests passed** summed over
  `build/suite_logs/*.log` — matching the report's number exactly.
- **ASan CA gate**: `build_asan/t6f_gate_asan.log` is real — generated `.*`-suffixed filter, 296
  suites, `1994 tests … [ PASSED ] 1994`, `GATE_EXIT=0`, binary linked 14:02, after the last
  C++-touching commit. Single-run rather than per-suite, but it reached the last suite in the list, so
  nothing was hidden behind an abort.
- **Soak scenario unit tests, my own run**: `46 passed`.
- **Integration**, verified in the nine surviving `clickhouse-server.log` files (not the harness log):
  zero `destructive work SUPPRESSED`, zero `no usable checkpoint` (the T6a carry), zero
  `unattributed`; real deletes in the sampled lanes (`gc_s3` 14 and 27, `ref_snaplog` 4/9/50,
  `relink` up to 18, `drop_pool_member` 1–4). 18 passed / 1 failed pre-fix, the failing lane 2 passed
  after `2a058cbd08c`.
- **`2a058cbd08c` non-vacuity**: the poll uses the file's own `RECLAIM_RETRIES`/`RECLAIM_SLEEP`, breaks
  only on `janitor_pending == 0`, and asserts `== 0` after the loop — so if the janitor never ran the
  assertion fails on an honest timeout rather than passing. `lifeless_keys=0` on every poll keeps the
  residue from being hard corruption on the way down, and `dangling`/`unaccounted` are still checked,
  so it does not mask a leak. The traded-away property (proof that the residue was *created*) is
  disclosed in both the commit message and the report; creation stays established by the asserted
  decommission/retire steps above it.
- **Binary fingerprints** (see PROSE finding 1) place both the unit-test binary and the server binary
  on the arc's `CasGc.cpp`.

## 9. Required follow-ups

1. TEST-1 — ground `buildKillShot`'s namespaces (or delete the arm) so the list_liar kill shot pins an
   attributable suppressor. Code/test → fix round.
2. TEST-2 — rename `HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` and repair the
   section header it belongs to. Code/test → same fix round.
3. The three PROSE items → `docs/superpowers/cas/deferred-docs-fixes.md`, no fix round.
