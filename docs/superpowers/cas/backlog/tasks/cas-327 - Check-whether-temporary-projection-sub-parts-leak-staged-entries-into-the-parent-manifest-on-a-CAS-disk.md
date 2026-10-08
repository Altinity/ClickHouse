---
id: CAS-327
title: >-
  Check whether temporary projection sub-parts leak staged entries into the
  parent manifest on a CAS disk
status: To Do
assignee: []
created_date: '2026-09-30 15:03'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:needs-measurement'
  - 'origin:review'
dependencies: []
references:
  - >-
    docs/superpowers/reports/2026-09-30-cas-part-removal-flow/02-design-inputs.md
priority: low
type: research
ordinal: 416000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Temporary projection sub-parts `<parent>/<p>_N.tmp_proj` are never removed on a CAS disk: `removeIfNeeded` sees them through the held transaction (`DataPartStorageOnDiskFull.cpp:63-72` at antalya-26.6 @e2dd0f5d343), but `remove` probes committed metadata, logs 'doesn't exist' and returns (`DataPartStorageOnDiskBase.cpp:981-988`). Not verified: whether the staged entries of the intermediate merge levels then end up in the published parent manifest (`MergeProjectionPartsTask.cpp:130-138`), i.e. whether a part with projections carries dead files.
Source: `docs/superpowers/reports/2026-09-30-cas-part-removal-flow/02-design-inputs.md`, 'Surprising' item 3.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A part with a projection built by a multi-level projection merge is inspected: its manifest lists no `.tmp_proj` entries, or the leak is shown with the manifest
- [ ] #2 If entries leak: a follow-up task with the fix shape
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
