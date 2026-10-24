---
id: CAS-169
title: Count S3 throttling (429 / 503 `SlowDown`) on CAS control writes by key class
status: To Do
assignee: []
created_date: '2026-09-02'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:observability'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - src/Common/ProfileEvents.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f26
priority: high
type: enhancement
ordinal: 215000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
No `ProfileEvents` split `SlowDown` / 429 on CAS control writes by key class (`_ckpt`, `ref_catalog`, `gc/state`, lease objects); the generic `S3WriteRequestsThrottling` has no key.
otel.demo: 537 throttling events in the week, 49-154 per day outside restarts; the audit could only infer by correlation that the throttled requests are the writers' `_log` / `_ckpt` / manifest PUTs (F26, PLAUSIBLE). `blob_storage_log` does not record throttled attempts that later succeed (F29).
Needed as the before/after measurement for A1 (`_ckpt`), spec B1 (GC LIST removal) and hot-key phase B (`ref_catalog`).
New events must pass the event rule of CAS-2 (recurring operating state that changes operator action); throttling per key class does.

Provenance: BACKLOG/gcs.md#environment [cas-throttling-by-key-class]. Verified 2026-09-26: no such event in src/Common/ProfileEvents.cpp at 8b87aa15d21 or 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Per-key-class throttling counters exist and a gtest on an injected `SlowDown` increments exactly the class of the key
- [ ] #2 `docs/en/antalya/cas/operations/monitoring.md` lists the new events
- [ ] #3 One soak or stand reading records the split, turning audit F26 into a measured fact
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
First recorded: 2026-09-02 (9f87032fde6, by 'cas-throttling-by-key-class')
<!-- SECTION:NOTES:END -->
