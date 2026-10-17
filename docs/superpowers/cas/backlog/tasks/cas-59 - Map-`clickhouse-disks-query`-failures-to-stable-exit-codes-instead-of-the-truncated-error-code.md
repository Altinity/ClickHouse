---
id: CAS-59
title: >-
  Map `clickhouse-disks --query` failures to stable exit codes instead of the
  truncated error code
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:tooling'
  - 'area:upstream'
  - 'complexity:trivial'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies: []
references:
  - programs/disks/DisksApp.cpp
priority: medium
type: bug
ordinal: 73000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`DisksApp::main` returns the raw ClickHouse error code as the process status (`programs/disks/DisksApp.cpp:622-623`), and POSIX keeps 8 bits.
Codes 256, 512 and 768 exist (`PARTITION_ALREADY_EXISTS`, `SET_NON_GRANTED_ROLE`, `CANNOT_EXECUTE_PROMQL_QUERY`, `src/Common/ErrorCodes.cpp:220,419,650`)
and exit 0: a failure that scripts read as success. Other codes above 255 report a mangled value.
The ca-soak harness and the CAS integration tests gate on this exit code. Map failures to a small set of stable nonzero codes before the change is carved out upstream.

Provenance: BACKLOG/operability-and-introspection.md#disks-exit-code-truncation item 1 (opus review M6); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every failing `--query` batch exits nonzero, including one whose error code is a multiple of 256
- [ ] #2 The exit-code set is documented in the code and tested
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
First recorded: 2026-08-22 (f07a9ed680d, by 'disks-exit-code-truncation')
<!-- SECTION:NOTES:END -->
