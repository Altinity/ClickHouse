---
id: DRAFT-20
title: >-
  Answer metadata and engine-trait queries from the nested storage once a lazy
  table materializes
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
milestone: m-5
dependencies:
  - DRAFT-18
references:
  - src/Storages/StorageProxy.h
  - src/Storages/StorageTableProxy.h
priority: medium
type: bug
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If the feature stays. `MATERIALIZE TTL` through a lazy proxy fails `INCORRECT_QUERY` "no TTL set": the proxy's cached metadata
carries columns only, and `getInMemoryMetadataPtr` is deliberately not overridden (`899fadfbcb4`). `isMergeTree` and
`supportsTTL` are not forwarded either (defaults `false`, `src/Storages/IStorage.h:109`, `:150`), so queries asking them get wrong
answers. Candidate rule: forward metadata and traits to the nested storage once materialized; needs its own consult.

Provenance: BACKLOG/operability-and-introspection.md#lazy-load-tables-decision-2026-07-21 (third bug) and [storageproxy-mergetree-virtuals-not-forwarded]; verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `ALTER TABLE ... MATERIALIZE TTL` succeeds on a lazy MergeTree table with a TTL
- [ ] #2 `isMergeTree` and `supportsTTL` answer as the nested engine does, before or after materialization, as the consult decides
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
First recorded: 2026-08-04 (f08734d17df, by 'storageproxy-mergetree-virtuals-not-forwarded')
<!-- SECTION:NOTES:END -->
