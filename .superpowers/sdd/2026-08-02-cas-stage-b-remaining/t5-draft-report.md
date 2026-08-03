# Task T5 draft report — delete probe A {#t5-draft-report}

**Status: UNVERIFIED-DRAFT.** Write-only lane: no build, no test run, no TLC, no praktika. Every claim
below about "what the code now says" is a grep/read verification against the working tree; every claim
about "what a test would assert if run" is derived from reading the assertions, not from executing them.

**Base commit:** `8e86c58a0f5de29a609e3dbc736164962b5b3227` (branch `draft/t5`, worktree
`/home/mfilimonov/workspace/ClickHouse/draft-t5`).

## Step 1 — the inventory {#step-1-inventory}

`git grep -nE "probe_a|ProbeA|ref_list_probe|sampleRefListQuality|RefScanDisagreements" -- .` at the
base commit produced 263 hits, saved at `build/t5_inventory_before.txt`. Classified:

**DELETE (production code)**
- `Gc::sampleRefListQuality` — decl + contract comment in `CasGc.h`, full definition in `CasGc.cpp`
  (was `:3795-3986` after the earlier extern-decl edits shifted line numbers from the audit's
  `:3739`/`:3809` baseline; confirmed by symbol, not line number, before deleting).
- The single call site in `Gc::runRegularRound` (the `PHASE 4/19 ref_list_probe` block) and its
  now-stale surrounding comment.
- The 6 `extern const Event` declarations in `CasGc.cpp` (`CasGcRefScanDisagreements`,
  `CasGcProbeAHolePresent`, `CasGcProbeAHoleAbsent`, `CasGcProbeADue`, `CasGcProbeAPerformed`,
  `CasGcProbeASkipped`) and their 6 definitions in `src/Common/ProfileEvents.cpp`.
- `PoolConfig::gc_probe_a_period` + its doc block in `CasPool.h`. Verified: no `DECLARE` in
  `ContentAddressedSettings.cpp`, no XML/DDL binding anywhere in the tree — it was a `PoolConfig`
  struct field only, read from `store->poolConfig().gc_probe_a_period` and set only in test fixtures.
- `ref_list_probe` dropped from `ContentAddressedGarbageCollectionLog.cpp`'s phase-order doc string and
  its `phase_metrics` example list; the user-facing doc row in
  `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` deleted.

**Stale comments fixed** — `CasGc.h:215`-area `RefScanSummary` doc (removed the "second enumeration ...
sampled store-quality detector" sentence entirely, since there is no second enumeration to describe
anymore); `CasGc.cpp` intake comment near `fold_ref_group` ("does not need a second opinion...") tightened
to drop the `sampleRefListQuality` cross-reference. `RefScanSummary`'s fields (`changed_shards`, `keys`,
`listed_lives`, `logs_by_life`, `max_log_by_life`) were checked one by one for a sole consumer being the
detector — **none qualified**: every field has another consumer (`defer_decision`'s `changed_shards`/
`namespaces_seen`/`ref_keys_listed`, the fold's `ref_object_keys`, `listed_lives` in the catalog cut,
`Gc.h:1005`'s test-only accessor). Nothing else was deleted from `RefScanSummary`.

**Step 2 — `enumerateRefPrefix`'s fate.** Confirmed at HEAD (post earlier-task edits, pre-T5): THREE
callers — `listRefPrefix`, `sampleRefListQuality` (now deleted), and the rebuild path
(`rebuildBaseline`). The helper is **kept**; the "collapse into `listRefPrefix`" option from the older
catalog plan is dead, per the audit's finding. No functional change to `enumerateRefPrefix` itself.

**Step 3 — phase renumbering, derivation table.** `grep -n 'GcPhaseTimer' CasGc.cpp` gives the true set
of instrumented sites. Cross-checked against `gtest_cas_gc_log.cpp`'s asserted execution order (19
names pre-deletion, including the uncommented `pre_fold_ref_drain` and `namespace_cleanup`, which the
old `PHASE N/19` comments never numbered — that convention is unchanged by T5). Post-deletion, 18
phases in true round order, with `PHASE N/18` comments on the 16 that carry one (2 = `pre_fold_ref_drain`
and 16 = `namespace_cleanup` stay uncommented, same as before):

| pos | phase | old comment | new comment |
|---|---|---|---|
| 1 | lease | PHASE 1/19 | PHASE 1/18 |
| 2 | pre_fold_ref_drain | (none) | (none) |
| 3 | heartbeat_floor | PHASE 2/19 | PHASE 3/18 |
| 4 | defer_decision | PHASE 3/19 | PHASE 4/18 |
| 5 | parent_seal_read | PHASE 5/19 | PHASE 5/18 |
| 6 | fold_ref_group | PHASE 6/19 | PHASE 6/18 |
| 7 | fold_seal_read | PHASE 7/19 | PHASE 7/18 |
| 8 | fold_ref_intake | PHASE 8/19 | PHASE 8/18 |
| 9 | fold_reduce | PHASE 10/19 | PHASE 9/18 |
| 10 | fold_seal_write | PHASE 11/19 | PHASE 10/18 |
| 11 | pending_deletes | PHASE 12/19 | PHASE 11/18 |
| 12 | meta_pool_wait | PHASE 13/19 | PHASE 12/18 |
| 13 | round_commit | PHASE 14/19 | PHASE 13/18 |
| 14 | handoff_reclaim | PHASE 15/19 (+ 1 backref) | PHASE 14/18 (+ 1 backref) |
| 15 | manifest_deletes | PHASE 16/19 | PHASE 15/18 |
| 16 | namespace_cleanup | (none) | (none) |
| 17 | ref_object_cleanup | PHASE 18/19 | PHASE 17/18 |
| 18 | orphan_sweep | PHASE 19/19 | PHASE 18/18 |

(Row 4, `ref_list_probe`, deleted outright — was `PHASE 4/19`.) The pre-T5 sequence had two structural
defects the audit named: 9/17 "missing" (an artifact of `pre_fold_ref_drain`/`namespace_cleanup` being
uncommented while the numbering assumed a contiguous 1..19) and 15 appearing twice under a naive
`grep -oE` (one declaration, one prose backreference — not a real duplicate declaration). Post-T5 the
same convention continues (2 and 16 uncommented) but is now internally consistent: every commented `N`
equals that phase's true 1-indexed position among 18. `docs/superpowers/cas/11-walkthrough.md`'s
`§13.2` mermaid diagram (`P1..P19`) was renumbered to `P1..P18` the same way, and its two textual
ordering call-outs ("16 after 14", "14's prune before the CAS") were corrected to "15 after 13"/"13's
prune before the CAS".

**Step 4 — the converted test.** `gtest_cas_retirement_sweep.cpp`'s
`TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` (which drove three separate pool
instances at `gc_probe_a_period` 2/1/0 and only checked round 1) is replaced by
`TheRoundEnumeratesTheRefPrefixExactlyOnce`: ONE pool, `gc_fold_max_defer_rounds = 0` (forces a fold
every round), 5 consecutive `gc.runRegularRound()` calls, each asserting `ref_prefix_lists == 1` against
a backend that counts `list()` calls matching `casRefsPrefix()` (`.../cas/ns/stream/`) by exact string.
Extended `RefPrefixListCountingBackend` with a second counter, `janitor_prefix_lists`, matching
`namespaceRootPrefix()` (`.../cas/ns/`) exactly, and asserted it is `> 0` every round — the "bounded
`cas/ns/` janitor page counted separately" requirement. Because the two prefixes are distinct exact
strings, the counters cannot conflate a hot scan with the janitor's own page by construction, not by
convention.

**Step 5/6 — the delta and the after-grep** are in their own sections below.

## Deviation: `gtest_cas_holey_list_detector.cpp` was NOT deleted whole {#deviation-holey-list}

The plan/audit's checkbox was "verify all 3 tests are probe-A-only first, record." Verification found
only ONE of three is probe-A-only:

- `TheHoleVerdictDistinguishesAMissedObjectFromAPhantomKey` — asserts on
  `CasGcRefScanDisagreements`/`CasGcProbeAHolePresent`/`CasGcProbeAHoleAbsent`. Probe-A-only. **Deleted**,
  along with the `probeAHoles`/`probeAHolesPresent`/`probeAHolesAbsent` helpers and the file's
  `CasGcRefScanDisagreements`/`CasGcProbeAHolePresent`/`CasGcProbeAHoleAbsent` extern decls (no other
  test in the file used them).
- `OmittedRemoveRecordIsSkippedForever` and `OmittedActivationNeverPermitsDeletingALiveBlob` assert on
  `blobPresent()` — whether the arithmetic ref-intake survives a holey LIST page (the retention-leak and
  data-loss halves of the skipped-transaction class) — with `probe_a_period` left at its production
  default (16, non-sampling), so probe A never fires during either. The file's own top comment already
  says the holey-list mechanism is used "only as the cheapest way to make the EFFECT executable,"
  independent of the detector. **Kept**, with `openHoleyPool`'s now-dead `probe_a_period` parameter
  removed and stale probe-A cross-references in the surrounding comments reworded.

Deleting the whole file as literally instructed would have silently dropped real regression coverage
for a data-loss/leak class that has nothing to do with probe A's removal. I flagged this to team-lead
mid-task (message sent, msg_id `e4ecc773-895e-4373-ac26-b8e3a17568bd`) and proceeded with the split
resolution rather than blocking, since the file's own docstring already made the independence explicit
and a wrong call here is cheap to revert. No reply had arrived by the time this report was written.

The file's name (`gtest_cas_holey_list_detector.cpp`) now describes a file with no detector in it; I did
not rename it (CMakeLists globs test sources, no explicit-name reference found, so a rename is cosmetic
and safely deferrable) — flagging the option rather than acting on it, since a rename touches a filename
that other in-flight lanes may reference by path.

## Steps 5/6 — expected release-gate delta {#expected-delta}

Relative to the immediately preceding commit of this lane (`8e86c58a0f5`), the CA gate test-count delta
this draft expects, **enumerated by name**:

**Deleted (4 tests, 0 whole suites):**
- `CasHoleyListDetector.TheHoleVerdictDistinguishesAMissedObjectFromAPhantomKey` (suite
  `CasHoleyListDetector` SURVIVES with its other 2 tests — this is the disclosed deviation above; the
  plan's estimate of "3 tests + 1 suite" does not hold under the corrected classification)
- `CasRetirementSweep.ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway`
- `CasRetirementSweep.TheDetectorsCadenceIsOnEveryFoldingRoundsRow`
- (the fourth deletion is folded into the conversion below, not a net test-count change)

**Converted (net zero test-count change, one test renamed and re-specified):**
- `CasRetirementSweep.TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` →
  `CasRetirementSweep.TheRoundEnumeratesTheRefPrefixExactlyOnce`

**Net expected CA-gate delta: −3 tests, 0 suites deleted**, versus the plan's estimate of "3 tests + 1
suite from the detector file, plus 2 deleted retirement-sweep tests" (= −5 tests, −1 suite). The
difference is entirely the holey-list-detector deviation: 1 test deleted there (not 3), and its suite
survives. The finisher should re-derive this exact number from `build/t5_gate.log` against
`build/t5_gate_before.log` at `8e86c58a0f5`, not trust this arithmetic — it is UNVERIFIED-DRAFT.

**Soak unit tests** (`python3 -m pytest utils/ca-soak/tests`, not run in this lane): all three named
consumer files updated coherently —
`test_checkpoint_signal_capture.py` (fixture drops `probe_a_holes`, assertions switch to
`ref_folding_aborted`), `test_metrics_signal_columns.py` (same substitution), `test_signals.py` (the
`_phase_row` default drops `probe_a_holes`; `test_phase_summary_sql_scopes_to_phase_rows_and_the_window`
drops it from the asserted SQL column list; `test_summarize_surfaces_the_detector_values` asserts
`fold_ref_list.ref_folding_aborted` instead of `fold_ref_list.probe_a_holes` and checks
`format_phase_summary`'s output for the same). No test was deleted in the Python suite — same count,
different assertions.

## Step 6 — after-grep {#step-6-after-grep}

Re-running the Step-1 grep (`build/t5_inventory_after.txt`, 166 hits, down from 263) leaves only:

- **Confirmed false positives**, unchanged: `gtest_cas_upload_fanout.cpp`'s `probe_acquired`,
  `gtest_cas_gc_shard_plan.cpp`'s local `probe_a` `ManifestId`, poco's
  `probe_and_set_default_ca_location`, `CaRefDeltaIntakeCore.tla`'s `ProbeAbsent` action (an unrelated
  TLA+ name).
- **Historical/dated documents left as history**, not live-state claims: `2026-07-28-stage-a-RESULTS.md`
  (a gate-battery results record, entirely past-tense measurement narrative — e.g. "Attempt 1... 49
  minutes under load: `CasGcProbeADue`... `= 0`" — describes what a specific run measured, not current
  behavior); `docs/superpowers/reports/2026-07-26-list-incompleteness-investigation.md` and
  `2026-07-26-s42-stale-edge-repro/raw/*.tsv` (raw captured soak data from a named incident);
  `docs/superpowers/plans/2026-07-24-...`/`2026-07-25-...`/`2026-07-28-cas-ref-chain-stage-b-catalog.md`
  (superseded plans, kept as provenance, not touched); `2026-08-02-stage-b-midpoint-audit.md` (the audit
  itself, describing what it found at ITS base commit — updating it would falsify the historical audit
  record it is).
- **Docs edited to add a correction note rather than rewrite history**:
  `2026-07-28-ref-rework-adjacent-findings.md` (R7's pre-existing SUPERSEDED note verified accurate,
  cross-reference to this task's plan added), `todo-20260726.md` (item 1 marked OVERRIDDEN, the "Probe
  A's two blind spots" finding marked MOOT), `BACKLOG.md` (`{#probe-a-cadence-unit}` marked MOOT,
  `{#gc-defer-decision-list-cost}`'s `ref_list_probe`-compounds-it clause corrected), `2026-07-28-stage-a-
  retirement-verdicts.md` (table rows 1/1b updated with the T5 verdict and corrected test citations, a
  SUPERSEDED note appended after the item-1 narrative).

No hit in the after-grep names a symbol that still exists in production code, test code, or Python.

## KEEP-list re-verification {#keep-list-reverified}

- **B1** (`logs_accounted == logs_applied` on `fold_ref_intake`) and **B2** (`produced=false` ordinals) —
  untouched; confirmed both have consumers unrelated to the detector (already established above for
  `RefScanSummary`; B1/B2 live in the intake phase, not in `sampleRefListQuality`).
- `Backend/CasProbe.h`, `Backend/CasSentinelProbe.h` — untouched, no reference to any deleted symbol.
- False positives (`probe_acquired`, `gtest_cas_gc_shard_plan.cpp`'s `probe_a`, poco's
  `ca_location` probe) — untouched.

## Open questions for the finisher {#open-questions}

1. **The holey-list-detector split** (above) needs a second opinion before the finisher trusts the
   test-count delta. If team-lead prefers the plan's literal whole-file deletion (accepting the lost
   retention/data-loss coverage), the finisher should re-delete `OmittedRemoveRecordIsSkippedForever` and
   `AHiddenRemovalStillReclaimsItsBlob`... but that coverage gap should itself be tracked, not silently
   accepted.
2. **Build/gate verification is entirely unperformed.** This lane did no compilation. The phase-renumber
   derivation table, the RefScanSummary field survey, and the enumerateRefPrefix 3-caller count were all
   done by reading and grepping, not by the compiler — a finisher should re-grep `GcPhaseTimer` and
   `enumerateRefPrefix` sites at their own HEAD before trusting the numbers above, since other lanes may
   have touched `CasGc.cpp` concurrently in this shared-worktree campaign.
3. **CHECKED, not open**: `gtest_cas_gc_log.cpp` has exactly one `expected` phase-order vector (the
   folding-round one already edited) plus one trivial DEFER-path check against `{"lease"}` — no second
   `ref_list_probe` mention exists in the file. Confirmed by grepping every `phaseNames(...)` call site.

## Commit {#commit}

Committed as one commit on `draft/t5`:
`ca: gc — delete probe A: no second full ref LIST per round (UNVERIFIED-DRAFT)`

Files touched (24): see `git show --stat` on that commit. This report is added via `git add -f`
alongside it, per dispatch.
