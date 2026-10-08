---
id: CAS-2.1
title: Remove or reroute the eleven `CASServer*` ProfileEvents that can never move
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp
  - src/Common/ProfileEvents.cpp
  - src/Disks/tests/gtest_cas_backend.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-2
priority: high
type: chore
ordinal: 7000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`classifyCasNs` (`CA/Backend/CasInstrumentedBackend.cpp:119-136`) returns only Blob, Manifest, Root, Gc and Other. The
`Server` row of `cas_event_table` (`:109-112`) and its eleven documented events (`src/Common/ProfileEvents.cpp:886-896`)
are unreachable: the per-server control subtree lives under `<prefix>/gc/server-roots/<srid>/` and classifies as Gc,
deliberately since `44e41878ff0` (pinned by `src/Disks/tests/gtest_cas_backend.cpp:304-308`).
Option A: delete the row and the eleven events. Option B: classify `/gc/server-roots/` as `Server` before the `/gc/` rule,
which moves mount/lease/epoch traffic out of `CASGC*` and is dashboard-visible, so it needs a deliberate call.
Mount and lease activity stays observable meanwhile through `system.cas_log` and `system.cas_mounts`.

Provenance: BACKLOG/operability-and-introspection.md#profileevents-surface-residuals (a); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No `CASServer*` event exists that the classifier cannot reach
- [ ] #2 The classifier gtest pins the chosen classification of `/gc/server-roots/` keys
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `Server` is 2026-06-07 (e46642b3237); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
