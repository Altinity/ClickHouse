---
id: DRAFT-8
title: Decide whether the per-blob `.meta` freshness marker can be folded away
status: Draft
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:45'
labels:
  - 'area:formats'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'touches:on-s3-format'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'needs:measurement'
  - 'origin:2031-triage'
dependencies:
  - CAS-11
references:
  - docs/superpowers/cas/2031-triage.md#cas-117
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f7
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The `.meta` sibling doubles blob object count and LIST enumeration. Idea: write it only when needed (for example only on `Condemned`).
It is protocol-adjacent: the marker carries freshness and the GC condemn state, and GC spends a PUT, a GET and a DELETE on it per garbage blob (audit F7).
Decision-4: any change ships as a new format version with a compatibility path. Decision-1 does not cover `.meta` but the same "instrument first, owner approves" rule applies.
The envelope's own open question is `formats-and-storage.md#blob-envelope-never-read-back` (u14).

Provenance: BACKLOG/performance.md#per-blob-meta-sibling-object-count (fold question; 2031-triage CAS-117). Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Owner go/no-go recorded, based on the FSCK body/`.meta` split from a real pool
- [ ] #2 If go: a spec names the format version and how old pools with `.meta` everywhere stay readable
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
First recorded: 2026-08-21 (10bf7f7162f, by 'per-blob-meta-sibling-object-count')
<!-- SECTION:NOTES:END -->
