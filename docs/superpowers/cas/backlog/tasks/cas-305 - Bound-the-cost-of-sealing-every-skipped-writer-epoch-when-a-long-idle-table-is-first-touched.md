---
id: CAS-305
title: >-
  Bound the cost of sealing every skipped writer epoch when a long-idle table is
  first touched
status: To Do
assignee: []
created_date: '2026-09-26 12:54'
labels:
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2031'
documentation:
  - docs/superpowers/cas/2031-triage.md
priority: medium
type: enhancement
ordinal: 384000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Writer recovery closes every dead epoch between the namespace's last seal and the live epoch with its own durable step. Each step is a conditional create of the seal at `{E, T+1}` (`op.create`, `R/Pool/CasRefLedger.cpp:1154`), then `publish_recovered_frontier` (`_ckpt` CAS with an exact read, `:1230`), counted by `CASRefRecoveryEpochSealed`. The walk is one loop (`:950-1230`) with no upper bound.
`writer_epoch` is per server root and is minted on every mount and on fence (`allocateWriterEpoch`), while the seal chain is per namespace. The first touch of a table idle across N remounts therefore costs O(N) sequential write pairs before the table is usable.
Per-epoch slot occupancy is forced by INV-1/INV-2, so the ask is to cut the cost per step or the number of steps, never to drop the seal.
Directions from the 2026-08-21 triage: (a) fold the `_ckpt` contributions of consecutive seals where the one-unfrontiered-transaction invariant allows, and skip the exact re-GET on intermediate epochs; (b) stop spending epochs where none is needed (overlaps DRAFT-29, `[fence-costs-epoch-distinct-mint]`); (c) log or count "N epochs sealed in one recovery" so the gap is visible before it costs minutes.
The ledger created this item as `{#recovery-seal-walk-per-skipped-epoch}` in `BACKLOG/ref-protocol.md` but never committed it, so no task carried it. Same code on antalya-26.6 (`CasRefLedger.cpp:1129`).

Provenance: docs/superpowers/cas/2031-triage.md#cas-114 (2031-triage CAS-114; the BACKLOG anchor #recovery-seal-walk-per-skipped-epoch it names was never committed, only 43b918b4ed6 mentions it); related DRAFT-29. Verified 2026-09-26 against cae9288ee65 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6). Code reading plus protocol analysis, not observed, so capped at Medium.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A recovery that seals more than one epoch reports the number of epochs sealed and its wall time in a log line or `ProfileEvents`
- [ ] #2 A measurement records first-touch latency and request count for a table idle across 1, 10 and 100 remounts, on the current code and after the change
- [ ] #3 The chosen reduction (fewer `_ckpt` publishes, fewer exact reads, or fewer epochs minted) keeps INV-1/INV-2; the ref-ledger gtests and the TLA+ recovery model still pass
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
