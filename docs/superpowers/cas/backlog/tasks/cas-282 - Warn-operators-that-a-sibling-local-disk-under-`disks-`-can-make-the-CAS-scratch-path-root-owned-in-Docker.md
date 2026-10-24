---
id: CAS-282
title: >-
  Warn operators that a sibling local disk under `disks/` can make the CAS
  scratch path root-owned in Docker
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
documentation:
  - docs/en/antalya/cas/configuration.md
priority: low
type: docs
ordinal: 349000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If a `local` disk declares a `<path>` under the same `<data-path>/disks/` tree as a CAS disk's default scratch path, the official
image's entrypoint `chown`s only that leaf; `disks/` stays root-owned, the CAS disk's scratch-dir creation fails with
`Permission denied`, and the server exits before listening. Fail-closed is correct; the ca-soak configs work around it.
`docs/en/antalya/cas/configuration.md:93` documents `cas_scratch_path` without the warning.

Provenance: BACKLOG/formats-and-storage.md#ca-scratch-path-docker-entrypoint; related CAS-102 (scratch sizing docs). Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The `cas_scratch_path` docs say to keep other local disks' paths outside `disks/<cas disk>/` or to set `cas_scratch_path` explicitly
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
First recorded: 2026-08-04 (b4420fe512a, by 'ca-scratch-path-docker-entrypoint')
<!-- SECTION:NOTES:END -->
