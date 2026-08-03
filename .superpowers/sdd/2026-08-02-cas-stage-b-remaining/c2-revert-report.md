---
title: 'C2 revert report — gc_round_manifest_cleanup_budget removed entirely'
date: 2026-08-03
branch: cas-gc-rebuild
---

# C2 revert: `gc_round_manifest_cleanup_budget` removed

## Why

`soak-t6b-report.md` showed the cap on the post-CAS `manifest_deletes` phase permanently leaks
the excess. `manifest_deletes` is a ONE-SHOT pipeline: the ref-log intake cursor that discovers
each owner-removed manifest's `-1` edge commits in the SAME round's CAS that produces
`folded.mf_cleanup`, before the deletes run — so a cap-declined entry is never re-derived by this
pipeline, and the orphan-manifest sweep backstop (~100 objects/round) cannot keep pace. Soak
numbers: run-1 (cap=5000) left 112,518 skipped / 110,218 permanently unreachable at checkpoint
(FAIL); run-2 (cap disabled) fully drained 223,714 with 0 unreachable (PASS). User decision: the
knob must not exist.

## Removal sites (all found, all removed)

1. `ContentAddressedSettings.cpp:83` — `DECLARE(UInt64, gc_round_manifest_cleanup_budget, ...)` removed.
2. `CasPool.h:120-124` — the `PoolConfig::gc_round_manifest_cleanup_budget` field + its comment removed
   (default was `5000`).
3. `ContentAddressedMetadataStorage.h:595` — `const uint64_t gc_round_manifest_cleanup_budget;` removed.
4. `ContentAddressedMetadataStorage.cpp` — the `extern` declaration, the member-initializer, and the
   `pool_config.gc_round_manifest_cleanup_budget = ...` pass-through in `openPool` all removed.
5. `Gc/CasBlobInDegree.h:314-324` — `GcRoundWorkBudget::max_manifest_cleanup_objects`,
   `manifest_cleanup_objects_used`, and `manifestCleanupAvailable()` removed, along with their comment
   block.
6. `Gc/CasGc.cpp` — the `round_work_budget.max_manifest_cleanup_objects = ...` seed line removed; the
   `manifest_deletes` phase's cap check (`if (!round_work_budget.manifestCleanupAvailable()) break;` plus
   the `manifest_cleanup_objects_used` increment) removed, so the loop now drains all of
   `folded.mf_cleanup` unconditionally. The phase's leading comment was rewritten to state the constraint
   (deliberately unbudgeted — a cap on the one-shot pipeline converts a bounded burst into a permanent
   leak) instead of describing the removed budget. The now-always-zero `skipped_budget` phase metric was
   dropped as dead (nothing else read it); `attempted`/`deleted`/`suppressed` metrics kept unchanged.
7. `src/Disks/tests/gtest_cas_gc_round.cpp` — the C2 cap test
   `ManifestCleanupBudgetCapsPerRoundDeletesAndLeaksToOrphanSweep` replaced by
   `ManifestCleanupDrainsEntireRoundWithNoSkips`, asserting the inverse property: a 5-manifest mass
   owner-removal drains all 5 bodies in the same round with none surviving.

## Failing-first evidence (chosen approach: temporary low-cap mutation, reverted before the real removal)

I wrote the new test (`ManifestCleanupDrainsEntireRoundWithNoSkips`) first, against the still-capped
code, then temporarily lowered `CasPool.h`'s `gc_round_manifest_cleanup_budget` default from `5000` to
`2` to force the cap to bite with only 5 manifests in the fixture. Built `unit_tests_dbms` (Release,
`build/ninja_unit_tests_c2_redcheck.log`) and ran the single test:

```
build/test_c2_redcheck.log:
  Expected equality of these values:
    rep.manifests_deleted
      Which is: 2
    kManifests
      Which is: 5
  manifest_deletes must drain the entire mf_cleanup vector in one round, not cap it
  [3x] "an unbudgeted cleanup must leave nothing surviving the round it was discovered in" — actual: true
  [ FAILED ] CASGcRound.ManifestCleanupDrainsEntireRoundWithNoSkips
```

Confirmed RED: with the cap forced to 2, only 2/5 manifest bodies were deleted and 3 survived the
round — reproducing exactly the leak class from the soak. I then reverted the `5000 -> 2` mutation and
performed the real removal (all sites above), rebuilt, and reran; the test now passes because the loop
no longer caps.

Note: the brief's "skipped/declined counter is 0" property is asserted indirectly —
`rep.manifests_deleted == kManifests` combined with all five bodies confirmed gone is equivalent to
"nothing was skipped," since there is no test harness for reading `GcPhaseTimer` per-phase metrics
directly. I did not build one; flagging this as a minor scope note rather than a silent gap.

## Gate results

- Release `unit_tests_dbms` build: clean (`build/ninja_unit_tests_c2_removed.log`).
- Release full CAS gtest run (`CAS*:CA*` filter, single invocation):
  `build/test_c2_full_cas_release.log` — 2004 tests, 0 failed.
- Release per-suite gate (`utils/cas-gate/run_cas_gate_per_suite.sh build`):
  `build/per_suite_results.txt` — `TOTALS: pass=278 fail=0 abort=0`.
- ASan `unit_tests_dbms` build: clean (`build_asan/ninja_unit_tests_c2_removed.log`).
- ASan full CAS gtest run: `build_asan/test_c2_full_cas_asan.log` — 2009 tests, 0 failed.
- ASan per-suite gate: `build_asan/per_suite_results.txt` — `TOTALS: pass=296 fail=0 abort=0`.
- Both per-suite gates run sequentially, not concurrently, per the known tmpfs-inode-exhaustion
  gotcha with two concurrent CAS gate runs.

## Grep closure

```
grep -rn "gc_round_manifest_cleanup_budget\|manifestCleanupAvailable" \
  --exclude-dir=docs --exclude-dir=.superpowers --exclude-dir=.git .
```
Zero hits tree-wide outside `docs/superpowers` and `.superpowers` (which retain only historical
references in past reports/plans, per the brief). `utils/ca-soak` (on MAIN) checked separately and
confirmed clean — zero hits.

## Commits

1. `1ac00e3c7a6` — `ca: gc — manifest cleanup is deliberately unbudgeted (a cap on the one-shot
   pipeline leaks)` — the 7 code/test files listed above.
2. `d63e0d1f5b3` — `docs: ledger + BACKLOG — mf-cleanup won't-cap decision and the durable-retry
   item` — `progress.md` (T6b entry, C2 clause rewritten) + `docs/superpowers/cas/BACKLOG.md`
   (new `[gc-mf-cleanup-durable-retry]` entry).

Both verified via `git show --stat HEAD` immediately after committing; each commit contains exactly
its intended file set.

## Deviations from the brief

- None in scope. One minor observability change beyond the literal ask: I dropped the
  `skipped_budget` phase metric in `manifest_deletes` (was `t.metric("skipped_budget",
  mf_cleanup_now.size() - attempted)`) since with the budget check removed it is structurally
  always `0` and nothing else in the tree reads it — keeping a "skipped_budget" metric name around
  after the budget is gone read as misleading observability debt. If preferred kept for
  dashboard/schema stability, it is a one-line re-add.
- The pre-existing working-tree `M` on `CasBlobInDegree.h` noted in the dispatch as unusual (git
  status showed it modified before I made any edits) turned out to be a false/stale signal — `git
  diff` on that file before my edits would have been empty; after my edits it contains exactly my
  intended removal and nothing else. Flagging in case it recurs elsewhere in the shared checkout.

## Concerns / open items

- The durable-retry redesign for real bounding (moving the edge-consumption point past the delete)
  is intentionally NOT attempted here — tracked as `[gc-mf-cleanup-durable-retry]` in BACKLOG,
  placed post-Stage-B alongside `gc-frontier-one-list`.
- `.superpowers/sdd/task-5-report.md`'s pre-existing modification (present at dispatch, unrelated
  to this task) was left untouched as instructed.
