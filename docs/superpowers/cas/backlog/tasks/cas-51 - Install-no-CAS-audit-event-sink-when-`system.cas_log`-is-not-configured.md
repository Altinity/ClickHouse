---
id: CAS-51
title: Install no CAS audit-event sink when `system.cas_log` is not configured
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: enhancement
ordinal: 61000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Removing the `<cas_log>` section is the documented way to disable the log, and `createSystemLog` then returns null
(`src/Interpreters/SystemLog.cpp:136-139`). But `makeCasEventSink` (`CA/ContentAddressedMetadataStorage.cpp:570-574`) returns a sink
whenever a context exists and checks for the log only inside the sink (`:587-589`). So `Pool::hasEventSink` (`CA/Pool/CasPool.h:865`)
says true, every emission site builds a full `CasEvent` and takes the dispatcher mutex, and the row is dropped at the end.
This contradicts the code's own claims (`CasPool.h:850-851`, `:863-864`, `CA/Pool/CasEventDispatcher.h:44`).
Wasted work only; nothing is lost. Fix: ask `getContentAddressedLog()` when building the sink and return an empty function when absent.
A one-shot check at pool open is enough: disabling the log is a static config choice. Event volume itself is a separate item (roadmap §3 "`cas_log` volume").

Provenance: BACKLOG/operability-and-introspection.md#cas-event-sink-installed-when-log-disabled (2031-triage CAS-104); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With the `cas_log` section absent, `hasEventSink` is false for a CAS disk and no `CasEvent` is constructed on the write path
- [ ] #2 With the section present, events reach `system.cas_log` as before
- [ ] #3 The comments in `CasPool.h` and `CasEventDispatcher.h` about the disabled path are true again
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
First recorded: 2026-08-21 (2583e3427aa, by 'cas-event-sink-installed-when-log-disabled')
<!-- SECTION:NOTES:END -->
