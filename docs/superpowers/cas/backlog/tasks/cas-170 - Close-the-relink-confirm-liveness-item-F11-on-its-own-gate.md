---
id: CAS-170
title: Close the relink-confirm liveness item (F11) on its own gate
status: To Do
assignee: []
created_date: '2026-09-02'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:replication'
  - 'area:gcs'
  - 'area:soak'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - tests/integration/test_cas_gcs_relink_liveness/test.py
  - 'https://github.com/Altinity/ClickHouse/issues/2310'
documentation:
  - docs/superpowers/cas/2026-09-04-gcs-soak-15min.md
  - docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md
priority: high
type: task
ordinal: 216000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two replicas starved each other on relink confirm on a slow control plane (data divergence, not data loss).
The fix (rule 3 of `CasRefLedger::confirmExactRef` refuses only for a queued or carved mutation of the exact ref, via `MutationScope`) is in: `a43e2ef89d3`, `f4c87d0a6d8`, `b400b8c467e` on cas-gc-rebuild; `5740d2a2953` + `bf77615fe0a` on antalya-26.6; released in `26.6.4.20001.altinityantalya`.
Validated by `CaRelinkConfirmCore.tla` under all sabotage flags, the `CAS*` gtests (`gtest_cas_confirm_exact_ref.cpp`), `tests/integration/test_cas_gcs_relink_liveness`, and issue #2310 (closed 2026-09-25).
What is not done is the item's own gate: a clean phase-3 GCS soak with all four chaos faults fired, then a two-hour closing soak.
The 2026-09-04 15-minute GCS soak (binary `03ccdd795d9`, contains the fix) passed correctness but exited 1 on the harness fixpoint bound and recorded no confirm counters.
Operational workaround today: alternate `SYSTEM STOP FETCHES` on one replica while the other drains, then swap.

Provenance: BACKLOG/gcs.md#relink-confirm-lane-livelock. Design and plan deleted in 05b2a33ff32 (landed). Verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every subtask is Done
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
First recorded: 2026-09-02 (d1c0b90c697, by 'relink-confirm-lane-livelock')
<!-- SECTION:NOTES:END -->
