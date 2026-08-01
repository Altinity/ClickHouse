# Task 5 Checkpoint 7.5b — immutable `RoundInput` and `RefPlan`

## Implemented boundary

`listRefPrefix` completes the one hot `LIST(cas/ns/stream/)`, reads the later catalog cut, and
constructs an owning `RoundInput`. `RoundInput` owns both the completed `RefScanSummary` and the
catalog snapshot, so its callers may reuse or change their temporary input containers without
changing the reconciled round. `runRegularRound` retains one `RoundInput` and builds exactly one
`RefPlan`; its DEFER and fold paths use const views. A healthy rebuild follows the same ordering and
uses the same sole `buildRefWalkPlan`; damaged-state rebuild preserves its no-catalog-mutation route.

`buildRefWalkPlan` is the only row-admission loop. It adds only `Live` and `Removing` catalog
incarnations, then enriches those existing rows by exact id from parent state, listing hints, holds,
checkpoint observations, and tail observations. `RefPlan` has a private default constructor, is
non-assignable, exposes const rows only, and has no mutable `foldState` or destructive
`releaseFoldStates` operation.

Fold state has two deliberately distinct immutable plan-derived views:

- `parentFoldStates` contains only rows that actually existed in the adopted parent seal. The fold
  uses it for baseline/cursor validation, preserving the distinction between no cursor and a default
  cursor state.
- `successorFoldStates` copies every catalog-admitted plan row into the mutable successor seal, so
  later coverage and cleanup updates never mutate the plan and every fold target has a successor row.

This separation fixed a regression found during review: deriving baseline cursor authority from the
full successor set incorrectly treated a catalog-only row as a parent cursor, bypassing
`CasRefGc.BaselineGuardRefusesWhenSnapshotSurvivesWithoutLogsOrCursor`.

## Test coverage and mutation evidence

- `CasGcRefWalkPlan.CatalogIsSoleRowAdmissionAuthorityAcrossOrdinaryAndRebuildInputs` retains the
  ordinary/rebuild admission matrix. It now also proves that a catalog-admitted no-parent life is
  absent from `parentFoldStates` but remains in `successorFoldStates`; changing fold to seed its
  successor from the parent view would remove that target.
- `CasGcRefPlan.RoundInputOwnsObservationsAndSuccessorStateCannotChangePlan` mutates the real source
  observations and catalog cut after constructing the production `RoundInput` and `RefPlan`, then
  mutates the result of the production `successorFoldStates` helper. Borrowing either source, making
  the plan mutable, or returning plan storage instead of a successor copy makes the assertions fail.
- `CasRefGc.CatalogAdmittedFreshLifeWithoutParentSeedsSuccessorSeal` drives an actual regular fold for
  a catalog-admitted fresh life that has no parent cursor, logs, or snapshot, then decodes the
  persisted successor seal. With the deliberate mutation that seeds the successor from
  `parentFoldStates` instead of `successorFoldStates`, it failed 1/1 with the expected `map::at`
  missing-successor-life exception; the restored code passes.
- The baseline-guard test is the focused behavioral regression proof for a catalog-admitted fresh
  life with no parent cursor. Replacing `parentFoldStates` in fold with the full successor set makes
  its expected `CORRUPTED_DATA` exception disappear; using the parent view only to initialize the
  successor would later omit catalog targets.
- Existing `CasGcRoundDefer.FoldAndDeferEachBuildExactlyOneCompletePostListWalkPlan` preserves the
  one-builder pin. The frontier and healthy-rebuild focused tests exercise the same round boundary.
- The pre-fold TLA+ gate continues to reject stale/pre-drain provenance paths; a standalone builder
  intentionally cannot infer freshness from snapshot bytes.

## Validation

| Gate | Result |
| --- | --- |
| Genuine compile-time RED for missing `RoundInput`/`RefPlan` API | Failed as intended before implementation |
| `ninja -C build unit_tests_dbms` | PASS (119 build steps) |
| Final focused admission/isolation/fold/defer/frontier/rebuild gate | PASS, 7/7 tests |
| Checkpoint 7.5a expanded CA filter | PASS, 1,754/1,754 tests, 260 suites, 2 disabled |
| `docs/superpowers/models/run_prefold_drain.sh` | PASS, 18/18 expected outcomes (16 violation witnesses, 2 green configurations) |
| `git diff --check` | PASS |

Logs are in `build/task5_checkpoint75b_*`.

## Self-review

`buildRefWalkPlan` has one production definition and exactly the ordinary and healthy-rebuild
construction sites. `foldState` and `releaseFoldStates` are absent. Fold no longer accesses raw
parent state: it derives both the baseline parent view and the mutable successor seed from the
immutable plan. No additional catalog `GET`, hot `LIST`, runtime-cache, maintenance, janitor, or
Task 13 change was introduced.
