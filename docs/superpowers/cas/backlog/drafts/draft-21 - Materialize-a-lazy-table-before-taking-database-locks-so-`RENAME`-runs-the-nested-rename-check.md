---
id: DRAFT-21
title: >-
  Materialize a lazy table before taking database locks so `RENAME` runs the
  nested rename check
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies:
  - DRAFT-18
references:
  - src/Storages/StorageTableProxy.h
  - src/Databases/DatabaseAtomic.cpp
  - src/Storages/StorageBuffer.cpp
priority: medium
type: bug
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If the feature stays. The `checkTableCanBeRenamed` forward (`7ab1fc15f4c`) was reverted: it materialized the proxy under
`DatabaseAtomic`'s non-recursive mutex, and a lazy `Buffer` resolves its destination via `DatabaseCatalog::getTable` in its
constructor, re-entering the database and deadlocking. The non-forward is documented at `src/Storages/StorageTableProxy.h:57`,
so a never-accessed lazy table bypasses the nested engine's rename restriction. Fix: materialize at the interpreter level before
any database mutex, then re-verify identities under the lock. For an upstream PR, call out that the kept
`checkMutationIsPossible` forward also changes `StorageTableFunctionProxy`.

Provenance: BACKLOG/operability-and-introspection.md#lazy-load-tables-decision-2026-07-21 (fourth bug + revert); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `RENAME`/`EXCHANGE` of a never-accessed lazy table whose engine forbids rename fails as it does for an eager table
- [ ] #2 A lazy `Buffer` rename, including cross-database `EXCHANGE`, completes without deadlock in a stateless test
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

Identifier trace: the earliest docs mention of `Buffer` is 2026-06-02 (c0a7046a3a7); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
