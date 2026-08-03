# Task 1 report — metrics `Cas*` -> `CAS*`, compare-and-swap tails -> `CompareSwap`

## Step 1 — exact map
- `metric_names.txt`: **165** names (`156` from `ProfileEvents.cpp` `M(Cas…`, `9` from `CurrentMetrics.cpp`) — matches the plan.
- `metric_map.sed`: **165** rules. All 13 special compare-and-swap-tail renames were verified present
  in the baseline before writing the map (`MISSING SPECIALS: []`).

## Pre-flight collision check (beyond the plan)
Extracted every `Cas[A-Z]…` identifier in `src/`, `utils/ca-soak/`, `tests/` and intersected with the
165 metric names, then re-listed each hit in a non-metric context (688 lines). Every one was a comment,
log/test string, or `LIKE`/SQL literal naming the metric — no production symbol shares a metric name.
No file-scoped handling was needed.

## Step 2 — application
- Target set: `git ls-files 'src/*.cpp' 'src/*.h' 'utils/ca-soak/*' 'tests/integration/*' | xargs grep -lP '\bCas[A-Z]'` = **310** files; **79** actually changed.
- `LIKE 'Cas%'` -> `LIKE 'CAS%'` in `dump_cas_metrics.py`, `scenarios/framework/observe.py`,
  `smoke_relink_validate.sh`; tree-wide re-grep for `LIKE 'Cas%'` returns nothing.
- The four async-metric format strings in `ServerAsynchronousMetrics.cpp` were NOT ProfileEvents and were
  untouched by the sed, exactly as the plan warned; edited by hand to `CASGcIsLeader_{}`,
  `CASGcPendingReclaim_{}`, `CASGcLastSuccessAgeSeconds_{}`, `CASGcWedgedNamespaces_{}`.
- Post-state: `M(CAS` = 156 in `ProfileEvents.cpp`, 9 in `CurrentMetrics.cpp`; `M(Cas[A-Z]` = 0 in both.

## Step 3 — descriptions where "CAS" meant compare-and-swap
Reworded 4 sites, not the 3 the plan lists: `CASRefCkptPublished` ("token-CAS" -> "token compare-and-swap"),
and "a recovery CAS-walk" -> "a recovery compare-and-swap walk" in `CASRefRecoveryEpochSealed`,
`CASRefRecoveryEpochSealAdopted`, **and `CASRefRecoveryStragglerAdopted`** — the plan under-counted the
"CAS-walk" phrasing by one; same phrase, same meaning, obvious cause.

The plan's Step-3 verification grep is vacuous: `grep -vE '^\s*[0-9]+:\s*M\(CAS'` drops every metric line,
so it cannot report anything. Replaced it with a parse of every `M(name, "desc")` description string,
classifying each `CAS` occurrence. Result: all surviving `CAS` in descriptions is either the feature used
adjectivally ("CAS other-object PUT requests", "CAS conditional-write HTTP attempts") or a cross-reference
to another metric's name (`CASBlobUploadFanoutTasks`, `CASGcRetiredSpared`, …). No compare-and-swap sense remains.

## Step 4 — verification
- Compare-and-swap identifiers all still present (files matching, `git grep -lw`):
  `casPut` 54, `CasOutcome` 32, `CasResult` 38, `GcMaintenanceCasOutcome` 3, `casGcMaintenanceState` 4,
  `kMaxCatalogCasAttempts` 1, `cas_result` 1, `CasWriteOutcome` 13, `CasUnresolvedReason` 6,
  `CasRequestBudget` 23, `CasRequestController` 12, `CasCreateOutcome` 3, `CasOverwriteOutcome` 8,
  `throwCasWriteRetryLater` 10, `throwCasTransientUnavailable` 6, `validateCasRequestBudget` 12,
  `cas_request_budget` 19. None zero.
- Old-metric-name grep (`CasBlobPut|CasGcHeadMiss|CasBlobCas|CasMetaCas|CasRootCas|CasDedupCacheHits`)
  over all tracked files: **no output**.

### Diff review (mandatory) — conclusion: no accidental rename
Two-sided token audit of `git diff -U0`:
- **Every added `CAS…` token maps back to a name in the 165-entry map or to one of the four async format
  strings — zero unexpected additions.** This is the load-bearing check: an accidental rename would have to
  introduce a `CAS`-token absent from the map.
- Removed lines mention 13 non-map `Cas…` tokens (`CasRefProtocol`, `CasRefLedger`, `CasFsck`,
  `CasPartWriteTxn`, `CasShardQueue`, `CasRefApplyPoisoned`, `CasBuild`, `CasObjectPut`,
  `CasWiringOpsDeathTest`, and the four `CasGc*_` format strings). Each was traced: they appear on lines that
  merely *shared* a line with a renamed metric (e.g. `CasPool.cpp` "attribute `CASBlobBodyPutAvoided` to the
  cache -- see CasPartWriteTxn.cpp"), so the whole line shows as -/+ while the token itself survives.
  Verified by diffing `git grep -n <token> HEAD` against the worktree: every count delta resolved to either
  such a shared line or to `.superpowers/sdd/task-5-report.md`.

### Not staged: a foreign pre-existing modification
`.superpowers/sdd/task-5-report.md` was already modified in the worktree at session start (another agent).
It is not in my target set and is excluded from the commit. Staging used an explicit 79-file list rather than
the plan's `git add -A src/ utils/ca-soak tests/integration`, because `utils/ca-soak/` and
`tests/integration/` carry untracked run debris (`soak*.db`, `p/`, `pool/`, sidecar scripts, `test_log.txt`).

### Deliberately left `Cas`-spelled (2 sites)
`StorageSystemContentAddressedMounts.h` ("supersedes the retired process-global `CasGcIsLeader` metric") and
`tests/queries/0_stateless/05010_content_addressed_mounts_gc_health.sh:7` ("the retired process-global
`CasGcIsLeader` / `CasGcPendingReclaimEntries` CurrentMetrics gauges"). Both name **removed** metrics —
`CasGcPendingReclaimEntries` has zero other occurrences in the tree. Renaming them would invent a spelling
those gauges never had. Task 8's second sweep grep already excludes the `CasGc` prefix, so neither will be
flagged; recording them here so the decision is on the record rather than rediscovered.

## Step 5 — build and tests
- `build/naming_t1_build.log` — `NINJA_EXIT=0`, `[690/691] Linking CXX executable programs/clickhouse`.
- `build/naming_t1_unit.log` — `./build/src/unit_tests_dbms --gtest_filter='Cas*' --gtest_brief=1` under
  `flock`: **2006 tests from 279 suites ran, 2006 PASSED**, exit 0 (2 disabled, pre-existing).
  Suites are still `Cas*` while the METRIC externs they reference are now `CAS*` — expected until Task 4.

---

## Addendum — stale ProfileEvent names in `utils/ca-soak` (scope addition, lead-approved)

Committed **separately** as a second commit, not folded into `bd12384c7ed`: the repo forbids `--amend`,
so "same commit" was not available. Same task, same files.

### The claim, verified before editing
`ApplyPoisoned` appears nowhere in `src/Common/ProfileEvents.cpp` or `CurrentMetrics.cpp`. The real event
is `CASRefNeedsRecovery` (`ProfileEvents.cpp` `M(CASRefNeedsRecovery, …)`), emitted by
`CasRefLedger::requireRecovery`. `gtest_cas_ref_snapshot_publish_ordering.cpp` states it outright:
"`Poisoned` is this task's plan's name for what the code spells `RefLaneState::NeedsRecovery`".
Because the name was never in the registry it was never in the 165-name baseline, so the exact-name sed
could not have reached it — as the lead predicted.

**This was live breakage, not cosmetic.** `utils/ca-soak/scenarios/BACKLOG.md` records the symptom:
S38 run `20260729T115218` *raised* — "counter probe … did not return `['CasRefApplyPoisoned', …]` — the
binary does not have these counters".

### Renamed (live tooling only)
`CasRefApplyPoisoned` -> `CASRefNeedsRecovery` in `soak/signals.py`, `tests/test_signals.py`,
`scenarios/tests/test_card_probe_failclose.py`, `scenarios/README.md`, and cards
`s38_late_put_injection.py`, `s42_alloc_faults.py`, `s43_same_uuid_recreation.py`,
`s44_rebirth_namespace_file_readers.py`.

Deliberately NOT touched: `scenarios/BACKLOG.md:3178` and `scenarios/RUN_HISTORY.md:488` — historical
observation/run records. Rewriting them would falsify what the run actually reported. `docs/superpowers/**`
and `.superpowers/**` are out of scope.

### The existence cross-check found a SECOND stale name of the same class
Extracted every quoted `CAS…`/`Cas…` string from all **112** `.py`/`.sh` files under `utils/ca-soak/` and
tested each against the **1975**-name post-rename registry (plus the four `CASGc*_` async prefixes).
Beyond the reported one it surfaced **`CasRefRecoverySealPublished`** (`s42_alloc_faults.py`
`_EVENTS_OF_INTEREST`) — also absent from the registry. Renamed to `CASRefRecoveryEpochSealed`.
*This mapping is an inference*, not a code-stated equivalence like the first one; it is safe because the
entry is collection-only — no assertion in the card reads it, so no verdict changes either way.

### Three further stale identifiers in the same card, each verified absent from `src/`
The "Poisoned" vocabulary around the event was stale plan-language throughout:
- `RefApplyState` -> `RefLaneState` (the real enum, `CasRefLedger.h`; its members are
  `Ready/Writing/Wedged/NeedsRecovery/Closed/Faulted` — there is no `Clean`, `ApplyPending` or `Poisoned`,
  so the `_EVENTS_OF_INTEREST` comment naming those transitions was rewritten).
- `CasRefLedger::poisonApplyState` -> `CasRefLedger::requireRecovery`.
- `_POISON_LOG_NEEDLE = "is POISONED at"` -> `"NEEDS RECOVERY at"`. **This one is executed**, not prose: it
  is a `system.text_log` needle that could never match. The correct text is read directly off the `LOG_ERROR`
  in `requireRecovery`. Corroboration-only by the card's own design, so no gate changes.
- Also corrected a comment in the same card that still described the `observe.events_snapshot` filter as
  keeping `Cas*`; that filter became `LIKE 'CAS%'` in Step 2 of this task.

### Cross-check residue — every remaining unresolved name accounted for
| Name | Site | Verdict |
|---|---|---|
| `CasRef`, `CasBuild`, `CasPool`, `CasText`, `CasProtocol` | `s41_wide_insert_baseline.py:112-114` | Not metrics — stack-frame symbol substrings for profile attribution, matching production C++ class/file prefixes that the Global Constraints keep unrenamed. Correct as-is. |
| `CasGood` | `tests/test_signals.py:86` | A deliberately invalid name in the test that asserts `signal_events_sql` rejects non-identifiers. Correct as-is. |
| `CasStore` | `s06_s08_manifest_parts.py:91` | **Open finding, left alone.** A `logger_name = 'CasStore'` text-log probe. Neither the logger `CasStore` nor its message `crossed soft limit` exists anywhere in `src/` — a dead probe of the same class, but a *logger* name, outside the ProfileEvent scope of this addition, and I could not verify a correct replacement because neither string survives in the tree. Flagged for the lead rather than guessed. Its card documents the probe as best-effort (records `None`/inconclusive when the count is unavailable), so it fails open, not closed. |

### Verification
- `grep -rn 'CasRefApplyPoisoned\|CasRefRecoverySealPublished' utils/ca-soak/` -> only the two historical
  records above.
- Cross-check re-run after the edits: no unresolved name remains except the four rows justified in the table.
- `python3 -m pytest tests/ scenarios/tests/ -q` in `utils/ca-soak`: **336 passed**.
