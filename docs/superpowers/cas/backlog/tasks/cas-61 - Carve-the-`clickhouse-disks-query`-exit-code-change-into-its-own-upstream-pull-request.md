---
id: CAS-61
title: >-
  Carve the `clickhouse-disks --query` exit-code change into its own upstream
  pull request
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:upstream'
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies:
  - CAS-232
  - CAS-59
references:
  - programs/disks/DisksApp.cpp
  - tests/integration/test_replicated_database/test.py
documentation:
  - docs/superpowers/cas/BACKLOG/docs-and-cleanup.md
priority: low
type: upstream
ordinal: 75000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`DisksApp::main` returns the failing command's code for non-interactive `--query` runs (`programs/disks/DisksApp.cpp:622-623`; guarded by
`query.has_value()`, so the REPL still exits 0). It rides in the CAS branch because CAS gating needs it, but it changes a shared tool for every user.
Facts the upstream PR must carry: within one `--query` batch the code is reset once and each failure overwrites it, so a later success does not
clear an earlier failure; `remove` throws on an absent path while `list` does not, so `remove` is the verb most likely to newly fail a caller.
It exposed a latent defect: `tests/integration/test_replicated_database/test.py::test_replicated_table_structure_alter` read `metadata_path`
after `DETACH DATABASE`, got an empty string, and its `remove` silently failed, so the scenario was never exercised; the fix reads the path first
and asserts it is non-empty. The 2026-08-03 sweep of every `--query` use in `tests/`, `ci/`, `utils/` found only that victim.

Provenance: BACKLOG/operability-and-introspection.md#disks-exit-code-upstream; facts from docs/superpowers/cas/upstream.md G-item (deleted by 85c95839160); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Coordinator: parent under the #refactor-group-g task if it exists; drop that dependency otherwise.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An upstream pull request contains only the exit-code change, the stable mapping, the test fix and a changelog line
- [ ] #2 The CAS branch no longer carries the change once the upstream PR is merged and synced
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Belongs to the Group G upstream carve-out parent CAS-232 (dependency link set; the parent was created after this task).

First recorded: 2026-08-04 (0a17684b9d2, by 'disks-exit-code-upstream')
<!-- SECTION:NOTES:END -->
