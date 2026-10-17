---
id: DRAFT-39
title: >-
  Build a crash-at-Nth-durable-write harness on `InMemoryBackend` the day a
  crash window is found by an incident
status: Draft
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:testing'
  - 'complexity:large'
  - 'risk:low'
  - 'confidence:speculative'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h
priority: low
type: feature
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Crash-consistency coverage is either a `SIGKILL` at a wall-clock moment (`test_cas_shared_pool`, `test_cas_drop_pool_member`, `test_cas_gc_sharded`,
ca-soak chaos), which lands in a window by luck, or a gtest that hand-builds one post-crash state. No wrapper aborts at the Nth durable write of a
protocol run, re-drives it and checks the invariant, although `InMemoryBackend` has the seams (`setHoldDeletes`, `injectAmbiguousWrite`,
`failNextWriteWith`: `CA/Backend/CasInMemoryBackend.h:97`, `:116`, `:142`). The recorded trigger is a crash window found by an incident rather than a test.

Provenance: BACKLOG/testing-and-ci.md#no-step-injecting-crash-harness; verified 2026-09-26 against 8b87aa15d21 (seam names in the source were stale).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An incident-found crash window exists before this is promoted
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
First recorded: 2026-08-21 (2583e3427aa, by 'no-step-injecting-crash-harness')
<!-- SECTION:NOTES:END -->
