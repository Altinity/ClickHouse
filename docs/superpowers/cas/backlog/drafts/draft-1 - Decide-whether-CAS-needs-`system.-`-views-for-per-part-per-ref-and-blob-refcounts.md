---
id: DRAFT-1
title: >-
  Decide whether CAS needs `system.*` views for per-part, per-ref and blob
  refcounts
status: Draft
assignee: []
created_date: '2026-07-03'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:observability'
  - 'complexity:large'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'needs:spec'
milestone: m-7
dependencies: []
references:
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Shipped introspection: `system.cas_gc_log`, `system.cas_log`, `system.cas_mounts`, and the `cas-fsck`, `cas-gc-dryrun`,
`cas-gc-rebuild` and `cas-inspect` CLI tools. Not built: per-part and per-ref `system.*` views, and a top-down
decode/traversal surface. Only `StorageSystemContentAddressedMounts` exists under `src/Storages/System` on both branches.
Open questions: which question does each view answer that `cas-inspect` cannot, and what does a query cost on a large pool.
Roadmap §3 asks for fewer and clearer system-table columns, which argues against adding tables without a named reader.

Provenance: BACKLOG/operability-and-introspection.md#b15-b99-b169-b159-system-views (INTROSPECTION-1/2); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records which views, if any, are built, with the operator question each answers and its S3 cost per query
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
First recorded: 2026-07-03 (45afcb8bb01, by 'B15/B99/B169/B159')
<!-- SECTION:NOTES:END -->
