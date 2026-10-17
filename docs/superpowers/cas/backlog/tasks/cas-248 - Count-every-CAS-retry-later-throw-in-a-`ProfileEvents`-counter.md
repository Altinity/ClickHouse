---
id: CAS-248
title: Count every CAS retry-later throw in a `ProfileEvents` counter
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - src/Common/ProfileEvents.cpp
priority: medium
type: enhancement
ordinal: 313000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`throwCasWriteRetryLater` and `makeCasWriteRetryLaterExceptionPtr` (`CA/Backend/CasRequests.cpp:92-100`) only call
`logCasWriteRetryLater` (`:85-89`), a `LogSeriesLimiter`-gated warning keyed on the logger name, so one line per 30 s prints
however many writes are refused. About 80 call sites (54 in `CasRefLedger.cpp`, 11 in `CasPartWriteTxn.cpp`). `CASRequestGaveUp`
(`CasRequests.cpp:798`) counts only engine give-ups, not the ledger, catalog or write-transaction refusals. Write contention
therefore cannot be trended or alerted on. One aggregate event at the throw; the event must name its reader per the CAS-2 rule.
The sibling "GC stopped vs not leader" gap is CAS-1.

Provenance: BACKLOG/docs-and-cleanup.md#retry-later-no-profile-event (umbrella review M13); related CAS-2 (event rule), CAS-50 (error context). Verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every retry-later throw increments one new `ProfileEvents` counter exactly once
- [ ] #2 A gtest drives one ledger and one write-transaction retry-later path and asserts the counter
- [ ] #3 The event description names the alert or dashboard that reads it
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
First recorded: 2026-08-21 (3c7cd13190e, by 'retry-later-no-profile-event')
<!-- SECTION:NOTES:END -->
