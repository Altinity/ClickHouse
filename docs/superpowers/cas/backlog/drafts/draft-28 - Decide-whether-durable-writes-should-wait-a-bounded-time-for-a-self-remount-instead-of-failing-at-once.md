---
id: DRAFT-28
title: >-
  Decide whether durable writes should wait a bounded time for a self-remount
  instead of failing at once
status: Draft
assignee: []
created_date: '2026-07-28'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:mounts'
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'needs:measurement'
milestone: m-8
dependencies:
  - CAS-150
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
priority: medium
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
During fence-to-remount (about 2 min core window plus tails on the msan lane) every durable write fails at once with 668/210
through `throwCasWriteRetryLater` (`CA/Backend/CasRequests.cpp:92`); there is no bounded wait on the write path.
Question: should a query write block up to N seconds during a self-remount, as a ReplicatedMergeTree write does across a Keeper
reconnect? Measure the remount-time breakdown with the logging task first, then decide N or reject the idea.

Provenance: BACKLOG/mounts-and-lifecycle.md#mount-fence [fence-window blast radius]; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The decision is recorded with the measured fence-to-Live distribution it is based on
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
First recorded: 2026-07-28 (1605b049a33, by 'fence-window blast radius')
<!-- SECTION:NOTES:END -->
