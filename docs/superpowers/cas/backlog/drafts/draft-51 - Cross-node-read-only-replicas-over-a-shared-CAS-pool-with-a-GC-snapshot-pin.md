---
id: DRAFT-51
title: Cross-node read-only replicas over a shared CAS pool with a GC snapshot pin
status: Draft
assignee: []
created_date: '2026-07-14'
updated_date: '2026-09-26 12:45'
labels:
  - 'area:gc'
  - 'area:mounts'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:decision'
dependencies: []
documentation:
  - >-
    docs/superpowers/specs/2026-07-14-cas-readonly-replica-snapshot-pin-design.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Design `docs/superpowers/specs/2026-07-14-cas-readonly-replica-snapshot-pin-design.md`: a third `Store::open` mode (`reader`) that
heartbeats a reader lease and publishes one pin object (`gc/readers/<reader_id>`); snapshot-sourced part discovery via the
upstream readonly-refresh MergeTree feature; an opaque `IDataPartStorage::snapshot_pin` so a running query's parts hold a GC
retention floor (min over live readers), bounded by query duration and reader-lease TTL.
None of `snapshot_pin`, `gc/readers/`, the `reader` mode or the `CaReadPinCore` TLA+ model exist. The ref snapshot+log layout
and pool-member decommission it depends on have landed. Read scaling, not a fix; no live gap asks for it.

Provenance: BACKLOG/replication.md#cas-readonly-replica; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records whether read-only replicas are on the roadmap, and the spec is re-checked against the current GC and ref layout
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
First recorded: 2026-07-14 (08e8a5f55ae, by 'cas-readonly-replica')
<!-- SECTION:NOTES:END -->
