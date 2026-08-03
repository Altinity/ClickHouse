# Task T5 finisher report — delete probe A {#t5-report}

**Status: DONE.** `git cherry-pick 8e036716d62` (the `draft/t5` probe-A deletion) applied cleanly onto
`cas-gc-rebuild` at `8274a730642` with **no conflicts** — git auto-merged `CasGc.cpp` and
`gtest_cas_ref_catalog.cpp` (the two files the dispatch flagged as tidy-touched) without any manual
resolution. Landed as `5b775616c36` (the pick's own subject/identity); this report is added and
committed on top, closing the task.

## Step 1 — inventory re-derivation {#step-1}

`git grep -nE "probe_a|ProbeA|ref_list_probe|sampleRefListQuality|RefScanDisagreements" -- .` at the
parent tip `8274a730642` (before the pick): **263 hits**, saved at `build/t5f_inventory_before.txt` —
identical count to the draft's own base (`8e86c58a0f5`), confirming no intervening T3-arc commit
touched probe-A code.

After the pick: **166 hits**, saved at `build/t5f_inventory_after.txt`. Every remaining hit is one of:
classified false positives (`gtest_cas_upload_fanout.cpp`'s `probe_acquired`,
`gtest_cas_gc_shard_plan.cpp`'s local `probe_a` `ManifestId`, poco's
`probe_and_set_default_ca_location`, `CaRefDeltaIntakeCore.tla`'s unrelated `ProbeAbsent` action), or
historical/dated documents correctly left as history (`2026-07-28-stage-a-RESULTS.md`'s past-tense
measurement narrative, superseded-plan files, the stage-b-midpoint-audit itself), or docs edited to add
a correction note rather than rewrite history (`2026-07-28-ref-rework-adjacent-findings.md`,
`todo-20260726.md`, `BACKLOG.md`, `2026-07-28-stage-a-retirement-verdicts.md`) — all verified directly
by reading each file's surrounding context, not merely counted. No hit names a symbol still present in
production code, test code, or Python.

## Conflicts and resolutions {#conflicts}

**None.** The cherry-pick auto-merged both files the dispatch anticipated conflicts in:

- `CasGc.cpp`: the tidy-fix arc's edits and the probe-A deletion touched disjoint regions; git's
  three-way merge combined them without a marker.
- `gtest_cas_ref_catalog.cpp`: same — the probe-A-era `ProbeSignature` `static_assert` deletion did not
  overlap any tidy-touched line.

Nothing required a "keep both intents" manual resolution because there was no textual overlap.

## Verification {#verification}

### Phase renumbering (Step 3) {#phase-renumbering}

`grep -n 'GcPhaseTimer'` / `PHASE N/18` in `CasGc.cpp` at HEAD: 18 phases, `PHASE 1/18` through
`PHASE 18/18`, no `/19` remnant, no `ref_list_probe` row — exact match to the draft's derivation table
(`lease`, `pre_fold_ref_drain` (uncommented), `heartbeat_floor`, `defer_decision`, `parent_seal_read`,
`fold_ref_group`, `fold_seal_read`, `fold_ref_intake`, `fold_reduce`, `fold_seal_write`,
`pending_deletes`, `meta_pool_wait`, `round_commit`, `handoff_reclaim`, `manifest_deletes`,
`namespace_cleanup` (uncommented), `ref_object_cleanup`, `orphan_sweep`). The
`content_addressed_garbage_collection_log.md` phase table lists the same 18 phases in the same order,
no `ref_list_probe` row. `11-walkthrough.md`'s `§13.2` mermaid diagram runs `P1`..`P18` with the two
corrected call-outs ("15 after 13", "13's prune before the CAS").

### `enumerateRefPrefix` (Step 2) {#enumerate-ref-prefix}

Confirmed at HEAD: exactly two callers (`listRefPrefix`, `rebuildBaseline`) — the detector's call site
is gone, the helper is kept as the draft specified.

### Exact by-name test delta {#test-delta}

Derived directly from `TEST`/`TEST_F` declarations diffed between the parent commit and HEAD for every
touched test file (source-level, not a build count):

| file | before | after | delta |
|---|---|---|---|
| `gtest_cas_holey_list_detector.cpp` | 3 tests (suite `CasHoleyListDetector`) | 2 tests, same suite | `TheHoleVerdictDistinguishesAMissedObjectFromAPhantomKey` deleted; suite survives (the disclosed deviation — verified correct, see below) |
| `gtest_cas_retirement_sweep.cpp` | 6 tests | 4 tests | `ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway` and `TheDetectorsCadenceIsOnEveryFoldingRoundsRow` deleted; `TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` renamed/re-specified to `TheRoundEnumeratesTheRefPrefixExactlyOnce` (net 0) |
| `gtest_cas_ref_catalog.cpp` | identical `TEST` list | identical | 0 (the `ProbeSignature` `static_assert` deletion isn't a `TEST`) |
| `gtest_cas_gc_log.cpp` | identical `TEST` list | identical | 0 (only the phase-order vector's contents changed, not test names) |

**Net: −3 tests, 0 suites.** Matches the corrected expected delta from the dispatch (and the draft's own
disclosed-deviation arithmetic) exactly.

The deviation (keeping `gtest_cas_holey_list_detector.cpp`'s other two tests,
`OmittedRemoveRecordIsSkippedForever` and `OmittedActivationNeverPermitsDeletingALiveBlob`) was verified
independently: both assert on `blobPresent()` against a holey-LIST-page fixture with `probe_a_period`
left at production default (non-sampling), never invoking the deleted detector. They exercise the
arithmetic ref-intake's resilience to a holey `LIST` page, a class unrelated to probe A's removal.
Deleting them would have silently dropped real retention-leak/data-loss regression coverage; keeping
them was the right call.

### Full release CA gate — measured before AND after, not inferred {#release-gate}

The dispatch asked for the expected count re-derived from the current tip before trusting any prior
report. The most recent recorded number (`t3-report.md`'s "278 suites, 1989 tests") turned out to
**not** match the parent tip exactly — `4e19cfe08e7` and `719c4d0ed87` (fsck fix-round commits) landed
*after* that measurement and added tests of their own, so 1989 was stale relative to `8274a730642`. Per
the standing rule against carrying forward a count something else can change, this finisher did not
use it. Instead: the touched production/test files were temporarily reverted to the parent commit's
content **in the working tree only** (`git checkout 8274a730642 -- <11 files>`, no branch switch, no
`HEAD` move), `unit_tests_dbms` was rebuilt incrementally (ninja recompiled only those files),
the gate was run for a true pre-pick measurement, then the files were restored
(`git checkout HEAD -- <11 files>`) and the binary rebuilt again to match the committed tree. `git
status` was clean before and after; this repo's working tree is currently unmodified relative to HEAD.

| | suites | tests ran | pass | disabled | log |
|---|---|---|---|---|---|
| **pre-pick** (`8274a730642`, temporarily-reverted tree, real build+run) | 278 | 1991 | 1991 | 2 | `build/t5f_gate_release_prepick_combined.log` |
| **post-pick** (HEAD, `5b775616c36`) | 278 | 1988 | 1988 | 2 | `build/t5f_gate_release_combined.log` |
| **delta** | 0 | **−3** | — | 0 | |

Exact match to the source-level named-test diff above. Per-suite runner (`utils/cas-gate/run_cas_gate_per_suite.sh build`, post-pick): **278/278 suites pass, 0 fail, 0 abort** (`build/per_suite_results.txt`).

### Full ASan CA gate {#asan-gate}

Post-pick, measured directly: **296 suites, 1993 tests ran, 1993 passed, 2 disabled**
(`build/t5f_gate_asan_combined.log`); per-suite runner: **296/296 suites pass, 0 fail, 0 abort**
(`build_asan/per_suite_results.txt`).

Pre-pick ASan was **not** independently rebuilt (a second full revert/rebuild/gate cycle on top of the
release one already run was judged not worth the wall-clock cost, given the release measurement already
gave an exact, non-inferred confirmation of the −3 delta). The −3 delta is expected to hold identically
for ASan: neither deleted/converted test (`gtest_cas_holey_list_detector.cpp`,
`gtest_cas_retirement_sweep.cpp`) carries any `#ifdef`/`DEBUG_OR_SANITIZER_BUILD` gating — confirmed by
grep, zero hits in either file — so the same three ordinary `TEST()` macros disappear from both build
types identically. Flagging this explicitly as a **derived, not measured**, pre-pick ASan number
(1996 tests, 296 suites) rather than presenting it as directly verified.

### The converted enumeration test {#enumeration-test}

Ran individually: `./build/src/unit_tests_dbms --gtest_filter="CasRetirementSweep.TheRoundEnumeratesTheRefPrefixExactlyOnce"`
— **PASSED**. The test drives 5 consecutive `gc.runRegularRound()` calls on one pool
(`gc_fold_max_defer_rounds = 0` forces a fold every round) and asserts, per round,
`ref_prefix_lists == 1` (matching `casRefsPrefix()` by exact string) and `janitor_prefix_lists > 0`
(matching `namespaceRootPrefix()` by a separate exact string). Because the two counters are driven by
disjoint exact-string comparisons in `RefPrefixListCountingBackend::list`, the janitor's own bounded page
cannot be conflated with a hot ref-prefix scan by construction — not by convention. This is Task 12's
before/after anchor and nothing in conflict resolution weakened it (there was no conflict to resolve).

### Python consumers {#python}

`python3 -m pytest utils/ca-soak/tests -q` → **290 passed** (`build/t5f_soak_pytest.log`). Zero
remaining `probe_a` references in `signals.py`, `metrics.py`, `run.py`, or the three test files (grepped,
zero hits).

### Doc/spec sync {#docs}

- `content_addressed_garbage_collection_log.md`: `ref_list_probe` row deleted, phase table's 18 rows
  match the enum exactly.
- `2026-07-28-ref-rework-adjacent-findings.md`, `todo-20260726.md`, `BACKLOG.md`,
  `2026-07-28-stage-a-retirement-verdicts.md`: each carries the T5 SUPERSEDED/MOOT/OVERRIDDEN note the
  plan asked for, verified by reading the surrounding paragraph, not just grepping the marker word.
- `2026-07-27-cas-ref-chain-complete-cut-design.md` §5 (`{#fold}`): "Probe A ... is deleted outright, not
  merely demoted" — correct, matches current code. Its §10 alternatives-and-history table still lists
  "widened probe A" among rejected past design alternatives — left untouched deliberately, since it is a
  historical record of a rejected alternative, not a live-state claim.

### KEEP-list re-verification {#keep-list}

- B1 (`logs_accounted == logs_applied` on `fold_ref_intake`) and B2 (`produced=false` ordinals): both
  present and untouched in `CasGc.cpp`, confirmed by grep.
- `Backend/CasProbe.h`, `Backend/CasSentinelProbe.h`: zero diff between parent and HEAD.
- False positives (`probe_acquired` in `gtest_cas_upload_fanout.cpp`, the shard-plan's local `probe_a`
  `ManifestId` in `gtest_cas_gc_shard_plan.cpp`, poco's `probe_and_set_default_ca_location`): all present
  and unaffected, confirmed by grep against the current tree.

## Commit {#commit}

The cherry-pick landed as `5b775616c36` carrying its own draft subject
(`ca: gc — delete probe A: no second full ref LIST per round (UNVERIFIED-DRAFT)`) since it applied with
no conflicts to resolve; this report is added via a second commit on top,
`ca: gc — delete probe A: no second full ref LIST per round`, which also removes the
`(UNVERIFIED-DRAFT)` qualifier's premise by recording the verification this report describes.

`git log -1` on `cas-gc-rebuild` after this commit: see below.
