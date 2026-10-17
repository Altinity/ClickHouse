---
id: DRAFT-29
title: >-
  Consider minting an epoch above every historical value when the epoch object
  is re-minted after a wipe
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:speculative'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
documentation:
  - docs/superpowers/models/CaCasMountCore_RESULTS.md
priority: low
type: research
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The mount TLA+ model's `FenceCostsEpoch` gap: after a wipe of the epoch object, the honest re-mint mints the literal 1 and can
collide with an `(uuid, 1)` pair GC already fenced. Minting `epochCeiling + 1` would close it in the model. In code,
`allocateWriterEpoch`'s absent-epoch branch mints 1 (`CA/Pool/CasServerRoot.cpp:693-694`) and there is no durable ceiling to
mint above; the model's own honesty note says its honest branch reaches states the code cannot. Needs a source of truth
for the ceiling before it is a task.

Provenance: BACKLOG/mounts-and-lifecycle.md#hygiene-residuals [fence-costs-epoch-distinct-mint]; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Either a durable ceiling source is identified and a design written, or the gap is recorded as accepted
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
First recorded: 2026-08-04 (f08734d17df, by 'fence-costs-epoch-distinct-mint')
<!-- SECTION:NOTES:END -->
