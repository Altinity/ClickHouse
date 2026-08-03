# T6 arc review — destruction enablement (Fable, independent full review)

Reviewer note: this review was performed from scratch per dispatch ("do not look for or trust any
partial verdict"). At write-time a completed review by the earlier reviewer was discovered already at
`t6-review.md` (timestamped 15:04, i.e. it finished after all); that file is PRESERVED untouched, and
this review is filed alongside it. The two reviews were produced independently and CONVERGE — same
verdict class, and this review's T-3 is the same defect as that review's TEST-1, found here by static
trace and confirmed against that reviewer's run log.

Scope: branch `laneg/t6-finish` in `/home/mfilimonov/workspace/ClickHouse/lane-g`, range
`73755caa6e5..2be5ba021ef` (6 commits), reviewed against the plan's T6 section and
`{#global-constraints}`; full diff plus each commit; all claims verified against the code at the tip.

**VERDICT: APPROVE — with ONE TEST finding (T-3) that per campaign policy opens a fix round; it does
not put the flip's safety in doubt. Plus two nonblocking TEST observations and a PROSE batch.**

## Gates (run by this reviewer, at tip `2be5ba021ef`) {#gates}

Both `unit_tests_dbms` binaries rebuilt incrementally at the tip
(`lane-g/build/t6_review_build_release.log`, `lane-g/build/t6_review_build_asan.log`, exit 0), gates
under `flock "$(git rev-parse --git-common-dir)/unit_tests.lock"` via
`utils/cas-gate/run_cas_gate_per_suite.sh`:

| Gate | Result | Log |
|---|---|---|
| Release CA gate | **278/278 suites PASS**, 0 fail, 0 abort; regen `0 unclaimed` (21 excluded) | `lane-g/build/t6_review_gate_release.log`, `lane-g/build/per_suite_results.txt` |
| ASan CA gate | **296/296 suites PASS**, 0 fail, 0 abort; regen `0 unclaimed` (3 excluded) | `lane-g/build/t6_review_gate_asan.log`, `lane-g/build_asan/per_suite_results.txt` |

Sanitizer sweep of every touched test file (`EXPECT_(ANY_)THROW`/`LOGICAL_ERROR`): the arc's only touch
on a throw-expectation is the mechanical `AuthoritativeForTest → Authoritative` rename inside a
pre-existing `EXPECT_THROW` (`CasGcShardIncarnation.DuplicateLifeIdStopsDestructiveRoundAndRebuild`).
No `LOGICAL_ERROR` expectation added or removed; the death-test split is not implicated.

## Checklist verdicts {#checklist}

### 1. Flip cleanliness — PASS, with a verified disclosure {#c1-flip}

The gate formula in `Gc::fold` is character-for-character the plan's normative three-term form, and the
only edit inside the suppression computation is `universe_authoritative = policy ==
UniversePolicy::Authoritative` (renamed enumerator; `kDefault = Authoritative`).

The flip commit is NOT only flip+rename: it also removes (declared in its message) the
`FrontierUnproven::NoCatalogEntry` arm, the `FrontierUnproven::AppendAboveFrozenTail` arm together with
the `WalkTarget::frozen_tail` machinery, and the `else frontier_proven = true` branch of the no-witness
exit. I verified each was genuinely DEAD at the base by hand-walking the base walk loop
(`git show 73755caa6e5:.../CasGc.cpp`): the ceiling test (`committed_through < expected → break`) sits
at the top of the loop, making both `committed_through < expected` conjuncts downstream unsatisfiable,
and `catalog_names_this_namespace` was a hardwired `const bool = true`. All three removals are
behavior-preserving. One additive metric (`frontier_complete` on `fold_reduce`) rides along;
`planManifestCursorPage`'s argument change (`policy == AuthoritativeForTest` → `universe_authoritative`)
is value-identical. Deviation from Step 1's letter, disclosed and safe — observation O-1, nonblocking.

### 2. O-2 trace: `planManifestCursorPage` — PASS {#c2-o2}

- Called only under `!suppress_destructive && manifest_sweep_list_budget_keys > 0`;
  `!suppress_destructive ⇒ frontier_complete ⇒ universe_authoritative`, so the recovery authorization
  is true exactly when the gate is open.
- The ordering obligation holds: candidates are LISTed and exact-GET-frozen (bytes + token) BEFORE
  `readAdoptedFoldSeal` and the `CasRefCatalog::read` cut; nominations consume ONLY the frozen
  observation (`chassert` presence; absent body ⇒ skip); no fresh GET after the cut; identity
  re-validated from frozen bytes (throws `CORRUPTED_DATA` on mismatch); rebirths lose `deleteExact` on
  token change; budget exhaustion resumes strictly after the last DECIDED key, `wrapped = false`.
- The newly-reachable call has an executing standing witness:
  `CasGcRoundDefer.FoldAndDeferEachBuildExactlyOneCompletePostListWalkPlan`'s catalog-GET count 2 → 3
  with the third reader named.

### 3. Dead-arms sum invariant — PASS {#c3-sum}

`FrontierDeficit::{total, describe, count}` updated consistently with the shrunken enum. The invariant
`Σ buckets == frontier_namespaces − frontier_proven` holds structurally: each walked target adds one
denominator row and exactly one of {proven, one named bucket} (`unproven_reason` initialized
`Unattributed`); budget-skipped namespaces are added in bulk to BOTH sides by the same
`intake_unprobed_budget` count; the `ref_folding_aborted` rewrite keeps the equality
(`proven = 0`, deficit reset to `fold_aborted = frontier_namespaces`).

### 4. Non-vacuity of the five suppressor arms — PASS for 3a/3b/3c; observation T-1 for arms 1/2 {#c4-arms}

- healthy + (3a): mutation M1 (`kDefault = StageA_Suppressed`) fails three tests including the (3a)
  arm's production-path CONTROL — verified in `lane-g/build/t6f_mut1_run.log`.
- (3b): mutation M2 (floor deleted) fails the arm with 9 real named deletes — verified in
  `lane-g/build/t6f_mut2_run.log`; post-mutation re-verify 68/68 (`build/t6f_postmut_verify.log`).
- (3c): pins ITS OWN suppressor — `frontier_unprobed_budget > 0` read off the round's row plus
  `frontier_proven < frontier_namespaces`; deleting the budget arm fails it.
- Per-family inertness is real: `expectEveryDeleteFamilyInert` asserts four named key-prefix families
  plus a catch-all `deleteTotal == 0` naming any unlisted family.

**T-1 (TEST observation, nonblocking).** Deleting `!report.anomalies.empty()` or
`!carried_holds.empty()` from `suppress_destructive` would fail NO test: on every in-fold path the
suppressor co-fires with frontier incompleteness (every intake anomaly exit sets an unproven reason;
the fold clamp mints a `ManifestBodyMissing` hold; a held namespace is unproven by definition; a
carried hold through a quiet round also records a companion anomaly). The two terms are structural
defense-in-depth and cannot be isolated without a code seam; the arm tests disclose this in their own
comments. Carry as a hygiene/T8 note, not a fix round.

### 5. Adversarial sample of fix-wave adaptations — 17 reviewed, one real finding (T-3) {#c5-sample}

Strictly-stronger restorations, verified against base: `gtest_ca_wiring.cpp`
(`unreachable` GT→EQ 0), `gtest_cas_gc_log.cpp` (deletion half restored: `saw_deleted`, ordering,
`total_deleted > 0`), integration `gc_s3`/`ref_snaplog`/`shared_pool`×2/`replicated_relink` (drain
asserts + right-reason cross-checks: `deleted > 0` on GC bookkeeping — with `entries_redeleted`
EXCLUDED from the sum, which hardens it; `gc_s3` also pins a fully-proven-frontier round and zero
self-reported suppressed destructive phases; `ref_snaplog` restores `preview_deletes=0`;
`replicated_relink` restores the reclaim-at-least-one soundness guard), `assertions.py` +
`test_leftovers.py` (permitted-family narrowing deleted, zero tolerance restored, boundary asserted
from both sides), s28_s33 (full D1 bound restored), stateless ×4 (banner-comment-only edits, yaml
entries gone). Correct posture conversions: `SuppressedRoundNominatesNothing` → explicit
`StageA_Suppressed` (suppression is its subject). Correct count adaptation: round_defer GET 2 → 3
(named). Mechanical renames: shard_incarnation, helpers, bounded_walk (whose numeric assertions are
unchanged). The adjudicated
`CasGcFrontierGate.HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` fix is sound: it
reads the gate's own verdict (`frontier_complete`/`suppress_destructive`) instead of adapting on a
guess, and keeps the load-bearing survival assertion; the implementor's §4.1 gating argument
(`mf_cleanup_now` empty under suppression; nominations gated) checks out against the code.

**T-3 (CODE/TEST — the fix-round finding; same defect as the predecessor review's TEST-1).**
`CasListLiarEndToEnd.AnEntirelyHiddenNamespacesEdgeIsRefusedByTheProductionDefault` is inert with
respect to the property its rewritten comment names. The comment claims survival "on member count
alone" (catalog-named `hidden` cannot prove its frontier). The fixture cannot produce that:
`buildKillShot` writes NO `_ckpt` for either namespace (`publishAt`/`writeRefLogTxnRaw` only
`casAdmitEntry` a catalog row — I traced the helpers), so BOTH namespaces exit at the
no-usable-checkpoint arm; `visible` never folds its `+1`/`−1`, the blob's observable in-degree never
reaches zero, and the round suppresses on the ANOMALY term (`0 of 2 proven; checkpoint_unusable=2` —
confirmed in the predecessor's run log `lane-g/build/t6rev_killshot.log`, which I verified on disk).
The test would pass unchanged if the universe term were deleted from the formula: its fault never
fires. The kill-shot property itself is NOT uncovered — the frontier-gate arms carry it at unit level —
so the flip's safety is not in doubt; but this end-to-end variant needs its fixture repaired
(recoverable `_ckpt`s so `visible` genuinely folds and the blob is genuinely condemned while `hidden`
stays catalog-named-but-unproven) and its comment corrected. TEST → fix round.

### 6. Closeout greps — PASS {#c6-greps}

Re-derived at the tip: `git grep -in "task 7b" -- src/ tests/ utils/` = 0; `STAGE-A RETURN
ITEM|STAGE-A CONTRACT` in code trees = 0; CA entries in `broken_tests.yaml` = 0; `+TODO/FIXME` in the
arc diff = 0; every remaining `kDefault` reference is the alias or an explicit test spelling; every
remaining `AuthoritativeForTest` hit is historical docs/plans/reports. Residual stale Stage-A prose in
untouched files → P-4.

### 7. drop_pool_member drain proof — PASS, with observation T-2 {#c7-drain}

Blob side proves draining beyond doubt: early-exit poll then HARD `final <= blobs_baseline`,
cross-checked by `deleted > 0` on GC's own bookkeeping. Janitor side polls to `janitor_pending == 0`
with a hard terminal assert, `lifeless_keys=0` on EVERY poll, `dangling=0`/`unaccounted=0` at the end.

**T-2 (TEST observation, nonblocking).** The janitor half cannot observe residue CREATION (the old
`>= 1` could): if creation or the `janitor_pending` counter regressed, the poll exits vacuously on the
first iteration. Disclosed in report §11; observing pending>0 pre-drain is racy by construction.
Acceptable; a janitor-delete bookkeeping cross-check would close it if a counter exists or is added.

### 8. Gates — PASS (see top). {#c8-gates}

## PROSE findings (batch to `docs/superpowers/cas/deferred-docs-fixes.md`) {#prose}

- **P-1 (FALSE, pre-existing).** `CasGc.cpp`, the comment above the unprobed-budget denominator merge
  claims "The `chassert` states the equality" — no such `chassert` exists; same text at base.
- **P-2 (IMPRECISE/stale, pre-existing, now internally contradictory).** `CasGc.cpp`'s
  `fold_ref_intake` metric comment still defines the universe as "hint ∪ sealed cursors ∪ catalog
  `Live`/`Removing` entries", contradicting the arc's own rewritten intake header ("EVERY
  `Live`/`Removing` row of this round's frozen catalog cut, and nothing else"). The code implements the
  latter.
- **P-3.** Subsumed into T-3 (the kill-shot comment's false mechanism claim is part of that fix).
- **P-4 (stale Stage-A framing, pre-existing, outside the closeout's marker classes).**
  `CasOrphanManifestSweep.h` ("In Stage A that is not an edge case"), `CasGc.cpp` retention-residual
  comment ("Stage-A staging contract, register R4" — also an internal-register citation), `CasGc.h`
  `RefCoverage` ("Stage-A sentinel"), `CasLayout.h` ("un-incarnated (Stage A) shape").
- **P-5 (report imprecision).** `t6-report.md` §7's "No edit was needed" for `bounded_walk` is true of
  the fix wave only; the draft commit did edit the file (comment retargeting + rename).
- The implementor's own five deferred items (§13) confirmed present and pre-existing; same batch.

## Report-vs-code spot checks {#spot}

Mutation logs exist and contain the quoted failures; post-mutation re-verify 68/68 on disk; §14's
"no `.cpp`/`.h` after the gated commit" verified (`a1686eb699a..tip` touches two Python files + the
report); the one conflict resolution (PHASE 14/18, citation-free prose) verified in the tip file;
closeout commit `a37f5fc7f81` matches its message (verification + one `(see BACKLOG)` deletion).

## Verdict {#verdict}

**APPROVE** for integration, with **T-3** (the inert list_liar kill-shot variant — identical to the
predecessor review's TEST-1) as the one CODE/TEST finding that opens a fix round; it does not
compromise the flip's safety (the property is pinned at unit level by the frontier-gate arms).
Nonblocking: T-1 (anomaly/hold terms not independently testable — structural redundancy), T-2 (janitor
drain creation-side vacuity window), O-1 (flip-commit scope beyond the plan's letter, verified
behavior-preserving), and the PROSE batch P-1/P-2/P-4/P-5.
