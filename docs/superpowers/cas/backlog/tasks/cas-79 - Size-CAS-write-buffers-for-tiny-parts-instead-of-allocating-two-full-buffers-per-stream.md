---
id: CAS-79
title: >-
  Size CAS write buffers for tiny parts instead of allocating two full buffers
  per stream
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - '>-'
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f18
  - 'theme:parity'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f18
priority: high
type: enhancement
ordinal: 104000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CaContentWriteBuffer::CaContentWriteBuffer` (`R/ContentAddressedTransaction.cpp:1854-1880`) allocates its own buffer, then a second `WriteBufferFromFile` spill sink, plus `create_directories` and a random temp path, for every stream.
Audit F18: 77.6 of 100 GiB sampled allocation batches carry a CAS frame, all part writers; median part 10 KB (`system.*`) and 1.5 KB (`claude_otel`). On the 2026-08-31 soak the two write-buffer constructors outranked the next CAS frame 200-fold.
Not a leak and not on the critical path: the write path waits ~31x more than it computes. Removing churn may move wall time by nothing; measure first.
Also unmeasured: per-file overhead beyond the double buffer (finalize closure, captured `owner` `shared_ptr`).

Residual from the write-path allocation audit: the emulated test backend copies header maps by value on the write path (test-only cost, no production impact); fold into the same pass or record the exact site.

Provenance: BACKLOG/performance.md#ca-write-buffer-allocation-concentration + #writepath-cost-txn-final [write-path-alloc-audit]; roadmap §2 'Write buffers for tiny parts'. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A scoped profile states what fraction of a blob write's wall time is buffer construction; under 1% closes the task as concentrated but not costly
- [ ] #2 If acted on: allocation bytes per small-part stream drop, measured on the same workload, with no insert-latency regression
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
First recorded: 2026-08-04 (0f266066bef, by 'write-path-alloc-audit')
<!-- SECTION:NOTES:END -->
