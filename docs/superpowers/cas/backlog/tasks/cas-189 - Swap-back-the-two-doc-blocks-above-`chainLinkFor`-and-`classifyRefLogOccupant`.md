---
id: CAS-189
title: Swap back the two doc blocks above `chainLinkFor` and `classifyRefLogOccupant`
status: To Do
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - R/Pool/CasRefLedger.cpp
priority: low
type: docs
ordinal: 246000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The `mine | successor's seal | foreign` adjudication comment, which explains the narrow `catch` of `CORRUPTED_DATA`/`UNKNOWN_FORMAT_VERSION`, sits above `chainLinkFor` (`R/Pool/CasRefLedger.cpp:124-137`).
The `prev_epoch_seal` grammar comment, which explains `chainLinkFor`'s epoch comparison, sits above `classifyRefLogOccupant` (`:168-179`). Each block describes the other function.
Re-derive each block from its function's code rather than moving text blind.

Provenance: BACKLOG/ref-protocol.md#orphaned-adjudication-comment. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792 (comment at :124 on both).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each of the two functions carries the comment that describes its own code
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
First recorded: 2026-07-30 (95d974072c4, by 'ORPHANED-ADJUDICATION-COMMENT')
<!-- SECTION:NOTES:END -->
