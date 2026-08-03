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
