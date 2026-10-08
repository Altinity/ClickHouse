---
id: CAS-268
title: >-
  Refuse outcome-log records without `kind`/`outcome` and enforce the `gc/state`
  line cap on encode
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:gc'
  - 'area:formats'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.cpp
priority: low
type: chore
ordinal: 333000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`decodeOutcomeLog` sets `kind` and `outcome` only when the keys are present (`CA/Formats/CasGcOutcomesFormat.cpp:105-111`),
so a record without them decodes as `Spared`/`Blob`. The only consumer is the round report's tally, so the impact is a skewed
counter. The fold seal already refuses incomplete records (`2bbcbb18683`).
`encodeGcState` checks only `gc_shards >= 1` (`CA/Formats/CasGcStateFormat.cpp:37-48`), while `decodeGcState` enforces a 64 KiB
`line_cap`; `encodeFoldSeal` calls `checkLineBytes`. The only variable field is a LIST key (~1 KiB), so it is unreachable today.
Both behavior-only, no wire-format change.

Provenance: BACKLOG/formats-and-storage.md#outcome-log-oc-not-required and #gc-state-encode-no-line-cap; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An outcome-log record missing `kind` or `outcome` is refused as `CORRUPTED_DATA`
- [ ] #2 `encodeGcState` refuses a line its decoder would refuse
- [ ] #3 gtests cover both
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
First recorded: 2026-08-21 (bab19a3de54, by 'gc-state-encode-no-line-cap')
<!-- SECTION:NOTES:END -->
