# Task T5 adversarial review — probe A deletion {#t5-review}

Reviewed at `857b5af19f2` (branch `cas-gc-rebuild`), commits under review `5b775616c36` (the
`draft/t5` pick) + `857b5af19f2` (finisher report). Base tip `8274a730642`. Every claim below was
checked against the code at HEAD, not against `t5-report.md`.

**VERDICT: APPROVE-WITH-NONBLOCKING.** The deletion is complete and correct, the enumeration anchor
is a real fence, the phase renumbering is consistent across all four surfaces, and the −3/0 delta
reproduces exactly. Three non-blocking findings: one CODE (a dead soak detector key that T5's own
report claims is live), one hygiene (two files force-committed into the gitignored `build/`), and
several PROSE items for the deferred-docs batch.

## 1. Deletion completeness — VERIFIED {#deletion}

Inventory grep re-run myself at HEAD:

```
git grep -nE "probe_a|ProbeA|ref_list_probe|sampleRefListQuality|RefScanDisagreements" -- src tests utils programs docs/en
```

Six hits, all three classified false positives and nothing else:

- `gtest_cas_gc_shard_plan.cpp` — local `const ManifestId probe_a` (shard-hash test), 2 hits.
- `gtest_cas_upload_fanout.cpp` — `probe_acquired` atomic (permit-exclusion test), 4 hits.
- `base/poco/NetSSL_OpenSSL/src/Context.cpp` — `poco_ssl_probe_and_set_default_ca_location`.

`docs/en` is clean. The unrestricted grep additionally hits only `docs/superpowers/**` (dated
history + correction notes), `.superpowers/sdd/**` (the reports themselves), and
`build/t5_inventory_*.txt` (see finding H1). No hit names a symbol present in production code, test
code, or Python.

KEEP list intact:

- `Backend/CasProbe.{h,cpp}` and `Backend/CasSentinelProbe.{h,cpp}` all present, zero diff.
- Probe B1/B2 accounting present in `CasGc.cpp` (the `logs_applied` single-advance site, the
  `TxnApplyLedger` round-local ledger, B1's contiguous-run recomputation, B2's `txns_unapplied`
  verdict). Untouched by the pick.
- `Gc::enumerateRefPrefix` **survived** — declaration and definition both present, with exactly
  **two** callers: `Gc::listRefPrefix` and the rebuild path (`rebuildBaseline`'s
  `rebuild_ref_scan`). The detector's third call site is the only one that died. Confirmed by
  `git grep -n enumerateRefPrefix -- src`.
- `PoolConfig::gc_probe_a_period` is gone from `CasPool.h`; the remaining `probe` hits in that file
  are unrelated (`gc_frontier_probe_budget`, the capability probe, `setInstallRegionProbeForTest`).
- The `ProbeSignature` / `ExpectedProbeSignature` compile pin in `gtest_cas_ref_catalog.cpp` was
  removed with its `static_assert`; the `fold` and builder pins survive.

## 2. The enumeration anchor — SOUND FENCE {#anchor}

`CasRetirementSweep.TheRoundEnumeratesTheRefPrefixExactlyOnce`. I read the fixture and ran the test
standalone (`build/t5rev_anchor.log`: 1 test, PASSED) as well as inside the full gate.

**Does it check what its comment claims? Yes.** `RefPrefixListCountingBackend::list` classifies by
**exact string equality** on the prefix argument — `prefix == refs_prefix` and
`prefix == janitor_prefix`, incremented into two separate atomics. The two prefixes are
`layout().casRefsPrefix()` (`cas/ns/stream/`) and `layout().namespaceRootPrefix()` (`cas/ns/`),
which are distinct strings, so the janitor's bounded page cannot be counted as a hot ref-prefix
scan. Because the classification is equality and not `starts_with`, the fact that one prefix is a
textual prefix of the other is harmless.

**Not vacuous.** Both counters are reset at the top of each of the 5 rounds and then asserted
`== 1` and `> 0`. If `casRefsPrefix()` ever stopped being the literal prefix the round passes to
`backend.list`, `ref_prefix_lists` would read 0 and the `EXPECT_EQ(…, 1u)` would **fail**, not pass.
The prefix the round actually uses is `layout.casRefsPrefix()` inside `Gc::enumerateRefPrefix`'s
`forEachListedKey` call — same expression the fixture is armed with. `ASSERT_TRUE(acquired_lease)`
and `ASSERT_FALSE(deferred)` per round guarantee each iteration really folded rather than
short-circuiting into DEFER, which is what makes 5 rounds five observations instead of one.

**Would it catch a second hot enumeration reappearing? Yes**, and across rounds, not just round 1 —
a regression that fired only on a later round (a cadence, exactly probe A's old shape) still trips
it, because the assertion is inside the loop.

**Scope limit worth knowing for Task 12 (non-blocking, no action needed on T5).** The counter is
per `list` *call* with that exact prefix. Two consequences: (a) if the fixture ever grew past one
LIST page, a legitimately paginated single enumeration would count >1 and fail — with one part it
does not, so the test is currently strict-but-correct; (b) a future change that replaced the one
full-prefix LIST with N *narrower* per-namespace LISTs would leave this counter at 1 and pass. The
anchor pins "how many times the round enumerates the whole ref prefix", which is exactly what its
comment says, and is the right before/after for a change that keeps or removes that full scan. It
is not a total LIST budget.

## 3. Delta accounting — REPRODUCED EXACTLY {#delta}

By-name `TEST`/`TEST_F` diff over `src/Disks/tests`, `8274a730642` → HEAD, computed independently:

removed (4):
- `CasHoleyListDetector.TheHoleVerdictDistinguishesAMissedObjectFromAPhantomKey`
- `CasRetirementSweep.ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway`
- `CasRetirementSweep.TheDetectorsCadenceIsOnEveryFoldingRoundsRow`
- `CasRetirementSweep.TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond`

added (1):
- `CasRetirementSweep.TheRoundEnumeratesTheRefPrefixExactlyOnce`

**Net −3 tests, 0 suites** — matches the report's by-name table row for row, and matches the plan's
approved detector-file deviation (keep tests 1-2, delete only the probe-A test).

Full release CA gate, run myself at HEAD under the shared lock:
`utils/cas-gate/generate_cas_suites.sh build` → **278 suites, 21 excluded, 0 unclaimed**; then
`build/src/unit_tests_dbms` over the generated filter → **1988 tests from 278 test suites ran,
1988 passed, 2 disabled, exit 0** (172.8 s), recorded in `build/t5rev_gate2.log`. The report's
post-pick numbers (278 / 1988 / 1988 / 2) reproduce exactly.
Its pre-pick 1991 I did not re-measure (it required the temporary working-tree revert the finisher
performed and documented); the −3 is independently confirmed by the by-name diff above, which is
the stronger evidence anyway.

`python3 -m pytest utils/ca-soak/tests -q` → **290 passed** (`build/t5rev_pytest.log`), matching.

**Gate-invocation hazard, found while reproducing (PROSE, plan text).** The Global Constraints
describe the full gate as "one run of `build/src/unit_tests_dbms` with the generated colon-joined
`--gtest_filter`". Taken literally that is wrong: `build/cas_suites.txt` holds **bare suite names**,
and gtest matches a filter term against the full `Suite.Test` name, so the colon-joined bare list
matches **zero tests and exits 0** — I did exactly this on my first attempt and got
`Running 0 tests from 0 test suites` / `GATE_EXIT=0`. `run_cas_gate_per_suite.sh` gets it right by
appending `.*` per suite. Any future gate run must append `.*` to each name. Worth a one-line fix
to the plan text; optionally `generate_cas_suites.sh` could emit the `.*` suffix itself, or the
gate wrapper could fail when 0 tests ran. Not a T5 defect — T5's own measurement was correct
(1988 is only reachable with the suffix).

## 4. Phase renumbering — CONSISTENT ACROSS ALL FOUR SURFACES {#phases}

The `GcPhaseTimer` construction sites in `CasGc.cpp`, in execution order, are 18:

`lease`, `pre_fold_ref_drain`, `heartbeat_floor`, `defer_decision`, `parent_seal_read`,
`fold_ref_group`, `fold_seal_read`, `fold_ref_intake`, `fold_reduce`, `fold_seal_write`,
`pending_deletes`, `meta_pool_wait`, `round_commit`, `handoff_reclaim`, `manifest_deletes`,
`namespace_cleanup`, `ref_object_cleanup`, `orphan_sweep`.

`PHASE N/18` comments in `CasGc.cpp`: 16 of them, numbered 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
14, 15, 17, 18 — every number maps to the phase above at that index, with no `/19` remnant and no
`ref_list_probe`. The two gaps (2 = `pre_fold_ref_drain`, 16 = `namespace_cleanup`) are phases that
carry no `PHASE N/M` comment at all; the parent commit had the same two gaps (at 9 and 17 under its
own corrupt numbering), so this is pre-existing and not a T5 regression. The report discloses both
as "(uncommented)".

Cross-checks, all exact:

- `gtest_cas_gc_log.cpp` `CasGcLog.FoldingRoundEmitsEveryPhaseInOrder` expects precisely that
  18-name vector in that order. The renumbering agrees with the executing assertion, not just with
  a reading of the source.
- `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` phase table: 18
  rows, same names, same order, no `ref_list_probe` row. Its `defer_decision` row now reads "the
  round's one enumeration of the ref prefix", consistent post-T5.
- `docs/superpowers/cas/11-walkthrough.md` §13.2 mermaid: `P1`…`P18`, same names, same order.

The parent's numbering was genuinely corrupt (it had `heartbeat_floor` as 2/19 while the emission
order puts `pre_fold_ref_drain` there); T5 fixed that as a side effect, and derived the fix from the
timer sites rather than from the old numbers. Confirmed by diffing the parent's comment set.

## 5. Python consumers — one real CODE finding {#python}

`pytest utils/ca-soak/tests -q` → 290 passed. Zero `probe_a` / `ProbeA` / `CasGcRefScanDisagreements`
references remain anywhere under `utils/ca-soak/`.

### C1 (CODE, non-blocking, pre-existing but T5 now depends on it): the `ref_folding_aborted` substitution is DEAD {#c1}

The dispatch asked whether the substitution is a real `DETECTOR_METRICS` entry **and whether the
soak would actually populate it**. It is a real entry; it would **not** be populated.

`utils/ca-soak/soak/signals.py`:

```python
DETECTOR_METRICS = (
    ("fold_ref_intake", "logs_accounted"),
    ("fold_ref_intake", "logs_applied"),
    ("fold_reduce", "txns_unapplied"),
    # Not a detector value but the verdict the detectors drive: the round refused to fold.
    ("fold_ref_list", "ref_folding_aborted"),
)
```

There is no phase named `fold_ref_list`. The phase that carries `ref_folding_aborted` is
**`fold_ref_group`** — `CasGc.cpp` does `ref_list_timer.emplace(phase_sink, "fold_ref_group")` and
then `ref_list_timer->metric("ref_folding_aborted", …)`. The variable is still called
`ref_list_timer`, which is how the stale name survives to the eye. `gtest_cas_gc_log.cpp` and
`gtest_cas_list_liar_end_to_end.cpp` both key on `"fold_ref_group"`.

Consequence in `summarize_phases`: the loop does `r = by_phase.get(phase); if r is None: continue`,
so a renamed **phase** is silently skipped and the key `fold_ref_list.ref_folding_aborted` is
permanently **absent** from the `detector` dict and from `format_phase_summary`'s line. Note the
asymmetry — the function *does* fail closed on a renamed **metric** (`if metric not in r["metrics"]:
raise SignalsUnsupported`, with a comment explaining that this exact silent-degradation already
happened once with `logs_intended` → `logs_accounted`). The fence covers one half of the name and
not the other, and the uncovered half has already moved.

Partial mitigation: `_DETECTOR_COLUMNS` extracts `ref_folding_aborted` as a scalar column on
*every* phase row, so `metrics.py` does still store the number against the `fold_ref_group` row.
What is dead is the `detector` summary key and the logged line — i.e. the operator-facing surface.

Blame: **not introduced by T5.** `git log -S` puts the `fold_ref_list` → `fold_ref_group` rename in
`ff9f36a056f` ("probe A demoted to detector"), which also left the pre-existing
`("fold_ref_list", "ref_folding_aborted")` entry (and the then-live
`("fold_ref_list", "probe_a_holes")`) pointing at the vanished phase. T5 removed one dead entry and
left the other.

Why it still matters here: T5 **chose this entry as the replacement observable** for the deleted
`probe_a_holes`, and `t5-report.md` presents the substitution as a live one. The three soak tests
that cover it (`test_signals.py`, `test_checkpoint_signal_capture.py`,
`test_metrics_signal_columns.py`) all build synthetic `_phase_row("fold_ref_list", …)` fixtures —
they fabricate a phase the server never emits, so they pass while proving nothing about the real
pipeline. This is the "passes because its fault never fires" shape.

Suggested fix (one line each, no design question): rename the tuple's phase to `fold_ref_group`,
update the three fixtures, and — the durable part — extend the fail-closed check to the phase name,
e.g. raise `SignalsUnsupported` when a `DETECTOR_METRICS` phase is absent while other `fold_*`
phases are present. Recommend routing this to the next fix wave rather than reopening T5, since T5
neither introduced it nor regressed it.

## 6. Conflict resolutions — BOTH INTENTS SURVIVE {#conflicts}

The report states the pick applied with **no conflicts**; verified structurally:
`git show --name-only` of the tidy commit `c0d2cad0dfd` and of the pick `5b775616c36` have an
**empty intersection**, so there was no file for the two intents to collide in, let alone a line.

Spot-checks anyway:

- `CasGc.cpp` `NOLINT` count: 4 at the parent, 4 at HEAD (`CasGc.h`, `gtest_cas_gc_log.cpp`,
  `gtest_cas_retirement_sweep.cpp`: 0 → 0). No tidy suppression was dropped.
- T3-arc symbols in `CasGc.cpp` (`throwIfAmbiguous`, `reconcileRefCatalogCut`) present.
- T5 deletions all present in the same files (`sampleRefListQuality` absent, `ref_list_probe` absent,
  phase comments renumbered).

Sanitizer sweep over every touched test file
(`grep -nE "EXPECT_(ANY_)?THROW|ASSERT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"`): clean with
respect to T5. `gtest_cas_holey_list_detector.cpp`, `gtest_cas_gc_arithmetic_intake.cpp` and
`cas_test_helpers.h` have zero hits. The three hits in files T5 touched are pre-existing and none is
a `LOGICAL_ERROR` site: `gtest_cas_retirement_sweep.cpp`'s
`AConfigStillAskingForTheMaterializationGraceIsRejected` throws from `loadFromConfig`'s unknown-key
path (a T3-arc test, `DB::Exception` rather than an exact code — imprecise but not T5's and not a
sanitizer hazard); `gtest_cas_gc_log.cpp`'s is a backend-fault round abort;
`gtest_cas_ref_catalog.cpp`'s `LOGICAL_ERROR` expectations sit behind the documented compile-guarded
death-test split.

## 7. The kept detector tests — CORRECT DEVIATION {#deviation}

`gtest_cas_holey_list_detector.cpp` keeps `OmittedRemoveRecordIsSkippedForever` (retention/leak
direction) and `OmittedActivationNeverPermitsDeletingALiveBlob` (deletion/data-loss direction).
Both are genuinely probe-independent: they arm `HoleyListBackend::omitFromNthListCall` on the
round's own walk (`nth = 0`) and assert on `blobPresent()`. Both carry the anti-vacuity guard
`ASSERT_TRUE(b->holeServed())` — so a run where the sabotage never fired fails instead of passing,
which is what makes `EXPECT_TRUE(blobPresent(...))` in the data-loss test mean something.
`openHoleyPool` now takes only the backend out-param, with the dead period argument gone. Deleting
these two would have dropped real regression coverage for both halves of the skipped-transaction
class; the disclosed deviation was the right call and matches what the plan approved.

The comment rewrites in `cas_test_helpers.h` and `gtest_cas_gc_arithmetic_intake.cpp` are handled
well: the permanent-omission choice previously justified by "keeps probe A quiet" is re-justified on
its own terms ("a lying store need not ever recover the key") rather than left as a dangling
reference to a deleted mechanism.

## 8. Independence from the foreign commits — CONFIRMED {#foreign}

`82b78f17e51`, `73755caa6e5`, `a4db6439d8b` each touch **only**
`docs/superpowers/cas/BACKLOG.md`. They sit *above* the pick in the log
(`5b775616c36` → foreign ×3 → `857b5af19f2`), and `git log 5b775616c36 --not 8274a730642` returns
the pick alone, so the pick's only ancestor is the base tip. The closure commit adds one file
(`t5-report.md`). Neither T5 commit depends on the foreign backlog edits; no C++, test, or Python
file is shared with them.

## Findings register {#findings}

| id | class | grade | item |
|---|---|---|---|
| C1 | **CODE** | non-blocking, pre-existing | `DETECTOR_METRICS` names phase `fold_ref_list`; the server emits `fold_ref_group`, so `ref_folding_aborted` is never surfaced in the soak's detector summary, and the three tests covering it use fabricated phase rows. `summarize_phases` fails closed on a renamed metric but not on a renamed phase. Introduced by `ff9f36a056f`, not by T5 — but it is the observable T5 chose as its replacement. See {#c1}. |
| H1 | **CODE (hygiene)** | non-blocking | `build/t5_inventory_before.txt` and `build/t5_inventory_after.txt` (429 lines) are **tracked** inside `build/`, which `.gitignore` line 15 excludes — they can only have been force-added. `build/` is every collaborator's local scratch dir; tracked files there mean a clean rebuild dirties `git status` and risks accidental staging. The reproducible-inventory rule asks for the hit list "as a versioned artifact (task report)" — `t5-draft-report.md` already carries the classification, so the dumps belong in the plan/report directory or nowhere. Recommend `git rm --cached` them in a follow-up. |
| P1 | PROSE | **FALSE** | `t5-report.md` {#commit}: claims the pick carries the subject `ca: gc — delete probe A: no second full ref LIST per round (UNVERIFIED-DRAFT)`. The actual subject of `5b775616c36` is `ca: draft — probe A deletion (UNVERIFIED-DRAFT, no runs)`. |
| P2 | PROSE | **FALSE** | `t5-report.md` {#test-delta}: the kept detector tests run "with `probe_a_period` left at production default (non-sampling)". At HEAD `gc_probe_a_period` does not exist — the sentence describes the pre-pick tree while asserting a property of the post-pick one. (It is also doubly wrong on "non-sampling": the old default 16 made round 0 due.) |
| P3 | PROSE | **IMPRECISE** | `gtest_cas_holey_list_detector.cpp`, `OmittedActivationNeverPermitsDeletingALiveBlob`: the anti-vacuity comment says the old "detector fired" oracle "is now a property of a different, sampled mechanism, pinned in `CasRetirementSweep`". Post-T5 there is no sampled mechanism; what `CasRetirementSweep` pins is the exactly-once enumeration, which is not a sampled detector. |
| P4 | PROSE | **IMPRECISE** | Plan `{#global-constraints}`: the full-gate recipe ("one run … with the generated colon-joined `--gtest_filter`") runs 0 tests and exits 0 as written, because `cas_suites.txt` holds bare suite names. Needs the `.*` suffix stated (or emitted by the generator). Reproduced live. |
| P5 | PROSE | **IMPRECISE** | `CasGc.cpp`: `pre_fold_ref_drain` (2) and `namespace_cleanup` (16) carry no `PHASE N/18` comment, so a reader counting comments finds 16 of 18. Pre-existing, disclosed in the report; worth closing while the numbering is fresh. |

P1-P5 → `docs/superpowers/cas/deferred-docs-fixes.md` per the batching directive; they open no fix
round. C1 and H1 are code/repo-state findings and want a follow-up commit, but neither is a T5
regression and neither blocks this slice.

## Evidence produced by this review {#evidence}

- `build/t5rev_gen.log` — suite generation: 278 suites, 21 excluded, **0 unclaimed**.
- `build/t5rev_build.log` — `ninja -C build unit_tests_dbms`, `NINJA_EXIT=0`.
- `build/t5rev_gate.log` — the **bad** invocation (bare names): 0 tests, exit 0. Kept as the
  demonstration behind P4.
- `build/t5rev_gate2.log` — the real gate: 1988/1988 passed, 278 suites, 2 disabled, exit 0.
- `build/t5rev_anchor.log` — the enumeration anchor standalone: PASSED.
- `build/t5rev_pytest.log` — `290 passed`.
