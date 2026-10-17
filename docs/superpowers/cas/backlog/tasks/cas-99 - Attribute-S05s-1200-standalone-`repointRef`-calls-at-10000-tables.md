---
id: CAS-99
title: 'Attribute S05''s 1,200 standalone `repointRef` calls at 10,000 tables'
status: To Do
assignee: []
created_date: '2026-09-01'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:write-path'
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
dependencies:
  - CAS-83
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
  - utils/ca-soak/scenarios/cards/s03_s05_scale.py
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
priority: medium
type: research
ordinal: 137000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S05 at `--scale full` (2026-09-01, 10,000 tables, one insert each) observed `CASRefRepoint` = 1,200 where the card expects 0 (`utils/ca-soak/scenarios/cards/s03_s05_scale.py:179-192`). It does not reproduce at `dev` or `ci`.
`CASRefRepoint` is incremented only in `CachedPartFolderAccess::repointRef` (`R/Parts/PartFolderAccess.cpp:545`), whose production caller is the committed-part branch of `ContentAddressedTransaction` (`R/ContentAddressedTransaction.cpp:421`).
Leading hypothesis: these are the `delete_tmp_*` repoints of part removal after merges (gc.md `[PART-REMOVAL-REPOINT]`, audit F2), which only appear once old parts expire inside the window. If so, the card's premise is wrong and the two findings are one.

Provenance: BACKLOG/performance.md#s05-standalone-repoints. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The operation behind the repoints is named from `cas_log` `ref_repoint` rows or a stack sample
- [ ] #2 Its count is shown to scale with tables, parts or GC rounds
- [ ] #3 Either the finding is merged into `[PART-REMOVAL-REPOINT]` and S05's verdict excludes that path, or a separate bug is filed
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
First recorded: 2026-09-01 (9deed66b471, by 's05-standalone-repoints')
<!-- SECTION:NOTES:END -->
