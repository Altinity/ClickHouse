---
id: CAS-307
title: >-
  Rerun S06 at HEAD and explain its last three reds, which report
  `CASManifestHead=0` as a failure
status: To Do
assignee: []
created_date: '2026-09-26 14:36'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/RUN_HISTORY.md
  - utils/ca-soak/scenarios/cards/s06_s08_manifest_parts.py
priority: low
type: task
ordinal: 386000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S06's newest `RUN_HISTORY.md` rows are red with no recorded cause: 2026-09-02 23:41 (d4d067291156) and 2026-09-03 00:13 and 00:23 (8b26f0872b0d), rows 714-717. A pass at 00:22 on the same sha sits between them.
The run reports (`runs/20260903T002301_S06_seed1/report.md`) fail only "scan issues no manifest HEAD", while printing `CASManifestHead=0` against an expected `== 0`.
All three runs predate 36789e7bbe0 ("S06 fences the reader path", 2026-09-03 02:24 +0200), which reworked this verdict (`utils/ca-soak/scenarios/cards/s06_s08_manifest_parts.py:320-326`). They are most likely failing-first runs made while that card change was being written.
No S06 run exists after that commit, so the card's current verdict is unmeasured. Under the no-known-reds rule, this row must be closed by a rerun or a root cause.

Provenance: utils/ca-soak/scenarios/RUN_HISTORY.md:714-717 (no BACKLOG.md entry records them); related earlier S06 records BACKLOG.md#S06-20260705T215757-1 and #S06-20260705T220753-1 are run records. Verified 2026-09-26 against 66087be0ffb (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S06 has a `RUN_HISTORY.md` row at or after 36789e7bbe0 with pass or fail
- [ ] #2 The three September reds carry a one-line cause in `RUN_HISTORY.md` (card authoring or a real defect with a linked task)
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
