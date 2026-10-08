---
id: CAS-100
title: >-
  Measure S01's upload RSS growth at three blob sizes and scope the verdict to
  the write path
status: To Do
assignee: []
created_date: '2026-09-01'
updated_date: '2026-09-26 14:44'
labels:
  - 'area:soak'
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:soak'
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s01_s02_huge_blob.py
priority: low
type: research
ordinal: 138000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S01 RSS growth during the upload was 0 at `ci` (512 MiB blob) and 2.228 GiB at `full` (8 GiB blob, 28%). It tracks the blob, so it is not pipeline noise.
The verdict is `growth < blob size` (`utils/ca-soak/scenarios/cards/s01_s02_huge_blob.py:150`), which only catches full materialization.
`Memory` samples (139, 43% in undecomposed pool frames): 42% in `SerializationString::deserializeBinaryBulkWithSizeStream` under `MergeTreeReaderWide::readData` and 6% in `ColumnString::shrinkToFit` inside `MergeTask`, i.e. the merge's String reads. Only 7% was in the CAS write path (`publishBlob`, `PartWriteTxn`).

Provenance: BACKLOG/performance.md#s01-rss-scales. Comparison point Altinity#2233 is closed. Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 RSS growth is recorded for three blob sizes and classified as proportional, bounded or above the blob
- [ ] #2 The share of growth in CAS write-path frames is reported for each size
- [ ] #3 S01's verdict measures the write path, or the card documents why whole-server RSS is kept
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
First recorded: 2026-09-01 (9deed66b471, by 's01-rss-scales')

Merged from u19a-soak (Measure S01's upload RSS growth at three blob sizes and scope the verdict to the write path): Facts to add to CAS-100 (2026-06-28 measurements, after the streaming `putBlob` fix ccfa687c373):
The residual ~2x insert peak comes from generic insert block buffering, not from the CA path. A 2 GiB single-part CA insert peaked at 4.33 GiB with default settings for both 512 x 4 MiB and 32768 x 64 KiB rows. With `max_block_size=1024, min_insert_block_size_bytes=32MiB`, the same insert peaked at 358 MiB, which is O(block) and constant in part size.
Memory-profiler attribution at 1 GiB showed the peak in the test's `randomString` column. The ColumnString grew to a 2 GiB power-of-two capacity. The other large allocator was `WriteBufferFromS3::allocateBuffer` multipart churn: 63 x ~16 MiB, freed per part. No blob-sized String allocation remained.
Verdict idea from the source: run S01 with a small `max_block_size` and assert that peak stays bounded as the blob size grows. That is a regression guard that can go red, unlike `growth < blob size`.

Merged from u19c-soak (Measure S01's upload RSS growth at three blob sizes and scope the verdict to the write path): Earlier data point that contradicts the "0 at ci" figure: run 20260717T033430_S01_seed1 at ci scale (512 MiB blob, binary
`cdac5ce8409c`, 2026-07-17) recorded peak RSS growth of 531 MiB during the upload and failed the `growth < blob size` verdict.
At dev scale (64 MiB blob) growth was 51 MiB on 2026-07-11. Include ci in the three-size measurement and explain the spread.
Provenance: utils/ca-soak/scenarios/BACKLOG.md#S01-20260717T033430-1.
<!-- SECTION:NOTES:END -->
