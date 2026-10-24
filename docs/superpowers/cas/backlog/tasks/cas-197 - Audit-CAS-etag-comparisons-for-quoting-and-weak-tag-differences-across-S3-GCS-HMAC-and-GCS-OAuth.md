---
id: CAS-197
title: >-
  Audit CAS etag comparisons for quoting and weak-tag differences across S3, GCS
  HMAC and GCS OAuth
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:backend'
  - 'area:gcs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'origin:review'
dependencies: []
references:
  - R/Backend/CasEtag.h
  - R/Backend/CasEtag.cpp
priority: low
type: spike
ordinal: 254000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A compatibility review against entity-tag grammar was flagged on 2026-09-02 and never filed; its scope was not recorded.
`PersistedEtag::matches` compares renderings textually (`R/Backend/CasEtag.h:85-88`), and `isIncarnationValue` accepts any non-empty, non-`*`, comma-free value (`R/Backend/CasEtag.cpp:10-27`). Nothing normalizes quotes or a `W/` prefix.
Questions: does each store return the same quoting on HEAD, GET and PUT responses? Can any return a weak tag? Do persisted tokens in manifests survive a store that changes quoting?
Persisted tokens are on-S3 format: a normalization change falls under decision-4.

Provenance: BACKLOG/ref-protocol.md#entity-tag-grammar-compat-watch (from random/todo.md item 14). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A short note records, per backend, the etag form on HEAD, GET, PUT and `If-Match` responses, from live runs or vendor docs
- [ ] #2 Any mismatch found becomes a follow-up task; none found closes this
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
First recorded: 2026-09-26 (cdbde71edf2, by 'entity-tag-grammar-compat-watch')
<!-- SECTION:NOTES:END -->
