---
id: CAS-28
title: Audit whether any reader resolves a blob without holding a ref
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:repro'
  - 'origin:review'
dependencies: []
references:
  - >-
    docs/superpowers/specs/2026-07-14-cas-readonly-replica-snapshot-pin-design.md
priority: low
type: spike
ordinal: 34000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Cross-node GC has no fence for a reader that holds no ref (R1/X1 ephemeral reader pin). For normal MergeTree this is covered: namespaces are per-server, `DataPart` lifetime holds the ref, and a live ref resolving to an absent object surfaces `FILE_DOESNT_EXIST` (INV-NO-DANGLE).
Before designing a pin, enumerate the read paths and show whether a ref-less reader exists at all (backup, `clickhouse-disks`, cross-node relink fetch are the candidates).

Provenance: BACKLOG/performance.md#read-write [R1/X1]. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written enumeration of every CA read entry point states which ref pins it
- [ ] #2 Either no ref-less reader exists (item closed with the enumeration) or a concrete path and a design task are filed
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

Identifier trace: the earliest docs mention of `DataPart` is 2026-06-02 (c0a7046a3a7); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
