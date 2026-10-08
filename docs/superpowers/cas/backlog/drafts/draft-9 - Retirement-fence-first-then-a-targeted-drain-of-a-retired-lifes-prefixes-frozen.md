---
id: DRAFT-9
title: >-
  Retirement fence first, then a targeted drain of a retired life's prefixes
  (frozen)
status: Draft
assignee: []
created_date: '2026-09-16'
updated_date: '2026-09-26 12:45'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:contested'
  - 'needs:decision'
  - 'origin:review'
dependencies:
  - CAS-29.9
references:
  - CA/Gc/CatalogLifecycleReconciler.h
  - CA/Pool/CasRefCkpt.cpp
documentation:
  - >-
    docs/superpowers/specs/2026-09-15-cas-gc-dead-namespace-debris-cleanup-design.md
  - >-
    docs/superpowers/reports/2026-09-15-cas-gc-dead-namespace-debris-cleanup-codex-reviews/
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Frozen by owner decision 2026-09-16 as "not designed". Idea: after an authoritative drain, delete a retired life's
`namespaceStreamPrefix` and `namespaceStatePrefix` directly instead of waiting for the global janitor page.
Three codex rounds found five unfenced readers or writers: `retired_lives` is observation, not proof; two GC actors can
overlap; `Removing` lives still have recovery readers that throw `CORRUPTED_DATA` over drained inputs; an in-flight
`_snap` PUT can land after the drain; bulk deletion is not all-or-nothing.
Unfreeze condition: a retirement-fence invariant designed on its own (who may read or write a retired prefix until which
durable event; recovery, publisher admission and DROP retry consult it), and only if B4 leaves the janitor unable to keep up.
Open owner question with no default: may a round's drain phase perform physical deletes at all.

Provenance: BACKLOG/gc.md#targeted-drain-of-retired-lives; also the open question in #janitor-page-hardcoded corrections.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner answers whether the drain phase may perform physical deletes
- [ ] #2 B4 measurements show whether the janitor keeps up; if it does, this draft is closed
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
First recorded: 2026-09-16 (cbf3fe14e36, by 'targeted-drain-of-retired-lives')
<!-- SECTION:NOTES:END -->
