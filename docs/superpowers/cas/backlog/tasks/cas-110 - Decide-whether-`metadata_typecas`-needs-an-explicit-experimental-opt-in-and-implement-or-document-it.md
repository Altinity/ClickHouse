---
id: CAS-110
title: >-
  Decide whether `metadata_type=cas` needs an explicit experimental opt-in, and
  implement or document it
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:mounts'
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
milestone: m-0
dependencies:
  - CAS-109
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp
  - docs/en/antalya/cas/index.md
priority: medium
type: task
ordinal: 148000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
CAS has no `allow_experimental_*` setting and the `"cas"` registration prints no warning
(`src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:219-245`); the practical gate is one config line.
`docs/en/antalya/cas/index.md:77` calls CAS experimental but does not say the config line is the opt-in. This is the
difference between "a user opted in" and "a user typed a metadata_type". The dynamic SQL path has its own gate
(`dynamic-cas-disk-gate`); this item is the static `storage_configuration` path. Opus review M11, P2.

Provenance: BACKLOG/operability-and-introspection.md#no-experimental-gate (opus review M11); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An owner decision is recorded: a gate (setting or loud registration warning) or the docs stating the config line is the gate
- [ ] #2 The chosen option is implemented, and `index.md`'s Status section matches it
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
First recorded: 2026-08-22 (8b9cd77b94e, by 'no-experimental-gate')
<!-- SECTION:NOTES:END -->
