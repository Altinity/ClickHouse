---
id: CAS-75
title: >-
  End the S3 read range at the granule boundary for column-subset reads of
  compact parts
status: To Do
assignee:
  - '@ilejn'
  - '@filimonov'
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:read-path'
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:issue'
  - 'theme:parity'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2332'
  - src/Storages/MergeTree/MergeTreeReaderStream.cpp
  - src/IO/ReadBufferFromS3.cpp
documentation:
  - docs/superpowers/cas/2026-09-15-s3-drain-remainder-research.md
priority: medium
type: upstream
ordinal: 100000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Issue #2332: concurrent `SELECT FINAL` over compact parts resets S3 connections, exhausts ephemeral ports and costs the mount lease with no store outage. The read stops mid-granule with up to `remote_read_min_bytes_for_seek` = 4 MiB left in the requested range, so the connection cannot be reused. The same happens on plain S3.
The drain-remainder patch (branch `fix/antalya-26.6/s3-drain-buffered-remainder`) recovers at most ~7.8 KiB, and 0 bytes once any body read happened; research ranks it third.
Ranked first: do not create the remainder. Bound the range via `MergeTreeReaderStream::adjustRightMark` (`src/Storages/MergeTree/MergeTreeReaderStream.cpp:246`) and its `setReadUntilPosition` call (`:270`) feeding `ReadBufferFromS3::setReadUntilPosition` (`src/IO/ReadBufferFromS3.cpp:500`). Upstream-portable, no vendored Poco change.
First step: measure where the over-long ranges come from on the 229-part repro (`DiskConnectionsReset` per query).

Provenance: BACKLOG/performance.md#s3-drain-remainder-read-range-fix. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 On the #2332 repro, `DiskConnectionsReset` per query drops to near zero with the fix, with query results unchanged
- [ ] #2 A stateless test or gtest pins the range end at the granule boundary for a column-subset read of a compact part
- [ ] #3 The patch is shaped as an upstream PR (motivation outside CAS stated)
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
First recorded: 2026-09-26 (597753102a9, by 's3-drain-remainder-read-range-fix')

Issue #2332 is assigned to ilejn and filimonov. Upstream issue https://github.com/ClickHouse/ClickHouse/issues/122103. Candidate patch by filimonov: https://github.com/filimonov/ClickHouse/commit/3e0dd5e279dd29ccfbb242e71787e3cd25bef17d. Regression-suite workaround: `net.ipv4.tcp_tw_reuse=1` on the ClickHouse containers.
<!-- SECTION:NOTES:END -->
