---
id: DRAFT-45
title: >-
  Defer committed-ref DDL operations to transaction commit through a
  `pending_ref_ops` overlay
status: Draft
assignee: []
created_date: '2026-07-16'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:ref-ledger'
  - 'area:write-path'
  - 'complexity:large'
  - 'risk:high'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The one-pipeline transaction overlay covers the part-build path only. Committed-ref DDL ops (`removeDirectory`,
`removeRecursive`, `republishRef`, `dropNamespace` for DROP/MOVE/RENAME TABLE) and verbatim files are durable at call time.
Interim risk: a DDL that applies a durable ref-op and then aborts before disk commit keeps the early drop. No motivating bug;
highest regression risk (empty-cover `commitTransaction` workaround, DROP/DETACH/ATTACH rollback). `pending_ref_ops` exists on
neither branch; the spec and plan (`2026-07-15-cas-txn-one-pipeline-design.md`, `2026-07-16-cas-txn-one-pipeline.md`) survive
only in git history.
First step before any code: audit whether any single CA transaction interleaves an overlay-deferred part op with an immediate
DDL or verbatim op in an order-sensitive way. A dangling or lost part in the DDL gates means pull this forward.

Provenance: BACKLOG/formats-and-storage.md [TXN-ONE-PIPELINE follow-up] (also orphaned-open cluster C-1118). Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The interleaving audit is recorded, and it decides whether the overlay is needed
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
First recorded: 2026-07-16 (03f6da7a1ec, by 'TXN-ONE-PIPELINE follow-up')

2031-triage CAS-005: residual = no multi-ref atomicity, no rollback of a committed-ref repoint, immediate-class DDL ops; the ledger (#cas-005) names this follow-up as the tracking item. The best-effort `noexcept` `dropRefIfMatches` rollback stays documented (`CA/ContentAddressedTransaction.cpp:553-558`); related CAS-180.
<!-- SECTION:NOTES:END -->
