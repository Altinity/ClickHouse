---
id: CAS-130
title: Remove internal plan and backlog tags from source comments in one pass
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - src/Common/ProfileEvents.cpp
  - src/Common/ThreadStatus.h
  - src/Disks/tests/gtest_ca_wiring.cpp
priority: low
type: chore
ordinal: 169000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
About 239 lines in 72 files under `src`/`programs` carry `B<number>` tags, 60 files outside `CA/` (regex over-matches, treat as
approximate), e.g. `src/Common/ProfileEvents.cpp:789` (B168), `src/Common/ThreadStatus.h:91` (B90). Eight `M-W` plan
references remain in `src/Disks/tests/gtest_ca_wiring.cpp:31,332,875,1029,1468,1520,1632` and `gtest_cas_layout.cpp:126`.
Policy: keep the reason, drop the provenance. One sweep, not a fix round per site; P2 because most hits are in shared code.

Provenance: BACKLOG/operability-and-introspection.md#internal-provenance-ships (opus review M8) and #b131-repo-hygiene-mw-sweep ([B131]); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No comment under `src`/`programs` cites a `B<number>`, `M-W` or `D-W1` tag, and each edited comment still states its reason
- [ ] #2 The sweep lands as one commit that touches only comments and builds
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
First recorded: 2026-08-22 (8b9cd77b94e, by 'internal-provenance-ships')
<!-- SECTION:NOTES:END -->
