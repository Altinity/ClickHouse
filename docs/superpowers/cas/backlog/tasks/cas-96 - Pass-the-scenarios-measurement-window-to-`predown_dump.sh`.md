---
id: CAS-96
title: Pass the scenario's measurement window to `predown_dump.sh`
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:soak'
  - 'area:tooling'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
references:
  - utils/ca-soak/scenarios/framework/cluster_boot.py
  - utils/ca-soak/scripts/predown_dump.sh
priority: low
type: chore
ordinal: 134000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`predown_dump.sh` accepts `--from/--to` and applies them as a `${WINDOW}` clause to every trace, `part_log` and `cas_log` query (`utils/ca-soak/scripts/predown_dump.sh:38-55`).
`cluster_boot.predown_dump` (`utils/ca-soak/scenarios/framework/cluster_boot.py:255`) passes only the label (`:271`), so every dump aggregates the whole server lifetime.
S23's `Real` aggregate was full of setup-phase CAS write frames that could not be attributed to the idle minutes.

Provenance: BACKLOG/performance.md#s23-idle-rss-growth item 3. Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A scenario card can pass its measurement window and the dump's `manifest.txt` records that window
- [ ] #2 S23 passes its idle window, and its `Memory` aggregate contains only samples from inside it
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
First recorded: 2026-09-26 (27654231df2, by 's23-idle-rss-growth item 3')
<!-- SECTION:NOTES:END -->
