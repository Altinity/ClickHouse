---
id: CAS-19
title: >-
  Tear down the CAS blob-upload pool after the global context in
  `clickhouse-local`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:write-path'
  - 'complexity:trivial'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - programs/local/LocalServer.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.cpp
priority: medium
type: bug
ordinal: 25000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`LocalServer::cleanup` resets the blob-upload pool (`programs/local/LocalServer.cpp:918`) before `global_context->shutdown` (`:922`), the reverse of `clickhouse-server` (`Server.cpp:1513`, final `SCOPE_EXIT_SAFE`) and `clickhouse-disks` (`DisksApp.cpp:673`). Present on both branches.
`blobUploadPool` returns a raw `ThreadPool &` (`R/Pool/CasBlobUploadPool.cpp:52`), so a merge still fanning out while the context shuts down uses a destroyed pool; a new fan-out gets a `LOGICAL_ERROR`.
Fix: reorder in `LocalServer::cleanup`; longer term a `shared_ptr` or use counter instead of call-order discipline. Not the backpressure item 2031-triage CAS-047.

Provenance: BACKLOG/performance.md#blob-upload-pool-teardown-order (umbrella review M6). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `clickhouse-local` shuts the blob-upload pool down after `global_context->shutdown`, matching server and disks
- [ ] #2 A `clickhouse-local` run that exits during a CA merge completes without use-after-free under ASan
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
First recorded: 2026-08-21 (e77a8472db9, by 'blob-upload-pool-teardown-order')
<!-- SECTION:NOTES:END -->
