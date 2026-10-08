---
id: CAS-104
title: >-
  Re-run S07's 20,000-column `OPTIMIZE FINAL` at full scale and attribute its S3
  connections
status: To Do
assignee: []
created_date: '2026-07-06'
updated_date: '2026-09-26 14:38'
labels:
  - 'area:read-path'
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:soak'
dependencies:
  - CAS-74
  - CAS-75
references:
  - utils/ca-soak/scenarios/cards/s06_s08_manifest_parts.py
priority: medium
type: research
ordinal: 142000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S07's 20,000-column `OPTIMIZE FINAL` stalled in an S3 retry storm caused by ephemeral-port exhaustion. Requests and connections scale with the column count.
No full-scale S07 run has happened since (`utils/ca-soak/scenarios/RUN_HISTORY.md`, last S07 rows dev/ci on 2026-08-31 and 2026-09-01).
Candidate causes are open in u05: 100-request keep-alive rotation and connection resets from over-long read ranges. The 2026-09-25 audit did not cover this workload shape.

Provenance: BACKLOG/performance.md#scale-findings [wide-part O(columns)] (+ orphaned 2026-08-04 triage duplicate). Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S07 at full scale completes or fails on HEAD with `DiskConnectionsCreated`, TIME_WAIT count and requests per column recorded
- [ ] #2 If it still exhausts ports, the dominant connection source is named and a fix task is filed
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
Merged from mounts-and-lifecycle.md (#2243 housekeeping, S07 re-rated): the same port-exhaustion condition took the mount lease down and discarded in-flight PartWriteTxns, so S07 is availability class, not cost only.

First recorded: 2026-07-06 (4abf5b743ed, by 'wide-part O')

Merged from u19b-soak (merge into CAS-104): From the soak ledger's wide-part entry (2026-07-06, S07 full, 20000 columns): `OPTIMIZE FINAL` sat at progress 0 for over 4 minutes; the server log showed `Cannot assign requested address` (errno 99) to RustFS, the S3 client at retry attempt 7 of 501, and about 5 PutObject per 5 s. The part itself committed at INSERT; only the merge stalled. Two fix directions CAS-104 does not name: batch the per-column HEAD/GET/PUT of one part, and note that `.bin`, mark files and `primary.idx` always take the blob route regardless of size, so 20000 tiny columns become about 40000 objects (inline-by-size placement is CAS-16). Deployment note to carry into the docs if the re-run still exhausts ports: widen `net.ipv4.ip_local_port_range` and cap S3 connections for very wide tables.
<!-- SECTION:NOTES:END -->
