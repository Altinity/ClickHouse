---
id: CAS-102
title: Document local scratch sizing and when to use S3-native staging
status: To Do
assignee: []
created_date: '2026-07-06'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - docs/en/antalya/cas/configuration.md
  - docs/en/antalya/cas/architecture/part-lifecycle.md
documentation:
  - docs/superpowers/cas/2026-08-31-scenario-repair-ledger.md
priority: medium
type: docs
ordinal: 140000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
With the default `staging_backend = local` (`R/ContentAddressedSettings.cpp:86`) every blob-class file spills whole to local scratch before upload, because its key needs the content hash. A 100 GiB merge spilled 93 GiB (108.4 GB observed); a part file larger than free scratch cannot be written.
S3-native staging avoids this but stays opt-in (user decision) and needs native same-store copy at mount.
Operator docs name the scratch path (`docs/en/antalya/cas/configuration.md:93`) but not the free-space requirement or the trade-off.

Provenance: BACKLOG/performance.md#scale-findings [scratch=full-part] (+ orphaned 2026-08-04 triage confirmation). Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `docs/en/antalya/cas/configuration.md` states that scratch needs free space for the largest part file being written concurrently, per disk
- [ ] #2 The docs say when to switch to `staging_backend = s3` and what it requires
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
First recorded: 2026-07-06 (4abf5b743ed, by 'scratch=full-part')

2031-triage CAS-046: the scratch = full part bytes half (`[scratch=full-part]`); guard and sweep half is CAS-125.
<!-- SECTION:NOTES:END -->
