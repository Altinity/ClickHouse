---
id: CAS-16
title: 'Design read-request reduction: inline-by-size placement and one-GET part open'
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:read-path'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:spec'
  - 'needs:measurement'
  - 'origin:review'
  - '>-'
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f9
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - 'theme:parity'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f9
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
priority: medium
type: design
ordinal: 22000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Reads of CA parts pay one GET per blob; small files are inlined into the manifest only by a file-type predicate (`partFileMustStayBlob`, `R/ContentAddressedTransaction.cpp:67`) under a fixed `INLINE_CAP = 1 MiB` constant (`:100`), not by size and not configurable.
Scope (B121/B202): inline by size (drop the file-type predicate, inline below ~512 KiB), weigh the wide-part medium-column regression, keep a `.bin` carve-out; cut per-blob GET cost; one-GET part open.
Stage-1 write-side view: ~239 PUT per part on the wide insert; folding marks and minor streams into the manifest would cut PUTs too. First confirm whether the threshold should be a setting or stays a constant.
Raising the inline share hits the aggregate 16 MiB inline cap, which has no spill path (`formats-and-storage.md#manifest-inline-budget-no-spill`).
Related, not superseding: audit F9 (part-folder view rebuilds), F15/F16 (LIST on directory probes, #2439). The opt-in file-cache disk covers re-read-heavy workloads.

Provenance: BACKLOG/performance.md#read-write [B121/B202/one-GET-open] + #writepath-candidates-post-stage1 item 3; an orphaned 2026-08-04 triage finding (DownloadPart/relink-fetch read cost) folded in as confirmation. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A design picks the inline rule (size threshold, carve-outs, setting vs constant) with measured PUT/GET counts and wall time before and after on the wide-insert and a read benchmark
- [ ] #2 The design states whether readers of format v1 accept the new placement unchanged (decision-4) and how it interacts with the 16 MiB aggregate cap
- [ ] #3 Per-part GET count on open is measured on current HEAD and the target is stated
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
First recorded: 2026-09-26 (0bf85b2d3ee, by 'B121/B202/one-GET-open')
<!-- SECTION:NOTES:END -->
