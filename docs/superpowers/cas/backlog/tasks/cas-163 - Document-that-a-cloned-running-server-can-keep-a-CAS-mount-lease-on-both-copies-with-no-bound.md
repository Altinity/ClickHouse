---
id: CAS-163
title: >-
  Document that a cloned running server can keep a CAS mount lease on both
  copies with no bound
status: To Do
assignee: []
created_date: '2026-09-01'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:docs'
  - 'area:mounts'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
documentation:
  - docs/en/antalya/cas/architecture/mounts-and-leases.md
priority: low
type: docs
ordinal: 209000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A VM snapshot restored twice gives two runtimes with the same `server_uuid`, lease state and `thread_local_rng` state
(`src/Core/UUID.cpp:14-15`), so they can build byte-identical renewal bodies. One lands; the other's precondition fails, and
byte-equality resolution reports `Committed` to it as well (`CasOperation`, `CA/Backend/CasRequests.cpp:1063-1069`). Both
extend authority, repeatedly. The token guard does not bound the overlap to one renew period.
Cloning a running server that holds a CAS mount is unsupported; the mount docs should say so, and design arguments should not
assume a bound that is not there. Relevant to `self-authored-mount-reclaim`, which also recognises bodies by bytes.

Provenance: BACKLOG/mounts-and-lifecycle.md#resolved-by-get-clone-overlap ([resolved-by-get-unbounds-clone-overlap]); verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `mounts-and-leases.md` states that cloning a running server is unsupported and why the lease does not fence the clone
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
First recorded: 2026-09-01 (19c1b74b903, by 'resolved-by-get-clone-overlap')
<!-- SECTION:NOTES:END -->
