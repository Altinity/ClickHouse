---
id: DRAFT-18
title: Decide whether to quarantine `lazy_load_tables` or fund its proxy remediation
status: Draft
assignee: []
created_date: '2026-07-21'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
milestone: m-5
dependencies: []
references:
  - src/Storages/StorageTableProxy.h
  - src/Storages/StorageProxy.h
  - tests/integration/test_cas_lazy_load_recovery/test.py
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: medium
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`lazy_load_tables` (upstream database setting, `src/Databases/DatabaseMetadataDiskSettings.cpp:17`) wraps tables in
`StorageTableProxy`; four bugs of one class so far (unforwarded `IStorage` virtual). A 2026-07-21 audit counted ~60 unforwarded
virtuals, ~45 that must forward, including `backupData` (silent empty BACKUP). No compile-time guard exists; swap-on-materialize
does not fix the class; catalog-entry laziness is the clean shape. Consultant: net-negative as implemented, quarantine.
CAS angle: the soak turned it off (`utils/ca-soak/soak/run.py:1419-1424`), so a transient S3 error at load again strands a
CAS table FAILED until restart. Options: quarantine, full forwarding sweep plus AST guard plus backup test, or catalog-entry
laziness. Until decided, treat every lazy-table symptom as this class first. Roadmap §6.

Provenance: BACKLOG/operability-and-introspection.md#lazy-load-tables-decision-2026-07-21 (audit report deleted in f5c01e88d01); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An owner decision picks one option and is recorded
- [ ] #2 If the feature stays, the post-fault `getNested` cost under churn is measured at a soak chaos checkpoint
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
First recorded: 2026-07-21 (3b96172df7d, by 'lazy-load-tables-decision-2026-07-21')
<!-- SECTION:NOTES:END -->
