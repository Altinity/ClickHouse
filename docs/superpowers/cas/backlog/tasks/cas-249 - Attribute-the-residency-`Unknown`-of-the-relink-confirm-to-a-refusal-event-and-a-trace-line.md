---
id: CAS-249
title: >-
  Attribute the residency `Unknown` of the relink confirm to a refusal event and
  a trace line
status: To Do
assignee: []
created_date: '2026-06-07'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:replication'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - src/Disks/tests/gtest_cas_confirm_exact_ref.cpp
  - 'https://github.com/Altinity/ClickHouse/pull/2364'
  - 'https://github.com/Altinity/ClickHouse/issues/2310'
priority: medium
type: enhancement
ordinal: 314000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Rule 2 (residency) of `CasRefLedger::confirmExactRef` (`CA/Pool/CasRefLedger.cpp:447-460`) returns `ConfirmAnswer::Unknown` for a
namespace absent from `ref_name_slots` or with no current runtime, before the `refuse` lambda (`:470`) that gives every other
refusal a `CASRelinkConfirmRefused*` event and a TRACE line. Cache-budget eviction, catalog-life removal and remount all erase
the slot the same way. Under `alter`-suite load (hundreds of short-lived tables on one pool) eviction is likely, so a field
`Unknown` here cannot be told from an unrecovered mount. After rule 3 was fixed (issue #2310), this is the next cause of
`Unknown` and the only invisible one. PR #2364 built exactly this (`CASRelinkConfirmRefusedTableNotResident` + two gtests, gated
green) and was closed unmerged 2026-09-16; neither branch has it. Fit the name into CAS-170.3's event merge.

Provenance: BACKLOG/issue-2310.md#open-items [attach-partition-cas-relink-residency]; absorbs the residency part of u12's merge from gcs.md#relink-confirm-lane-livelock ('no refusal counter observed live'); the live-observation criterion stays in CAS-170.1. Related CAS-170.3, CAS-123. Post-fix GCS baseline: 230/230 `yes`, zero `unproven`, vs 2999 `unproven` in two hours pre-fix. Verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A residency refusal increments a `CASRelinkConfirmRefused*` event and logs the reason at TRACE, like every other rule
- [ ] #2 gtests in `gtest_cas_confirm_exact_ref.cpp` cover the absent-slot and no-runtime arms
- [ ] #3 A rerun of `alter_attach_partition_cas` part 1 reports per node the `Relink confirm is unproven` count and every `CASRelinkConfirmRefused%` value (`system_events_show_zero_values = 1`)
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
First recorded: 2026-06-07 (07a483c65e0, by 'open-items')

Issue #2310 was closed on 2026-09-25 (rule-3 fix released in 26.6.4); this residency `Unknown` is its remaining open cause and is not covered by the closure.
<!-- SECTION:NOTES:END -->
