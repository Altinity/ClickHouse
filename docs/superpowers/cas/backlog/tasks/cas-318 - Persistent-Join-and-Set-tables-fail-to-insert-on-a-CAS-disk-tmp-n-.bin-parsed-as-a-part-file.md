---
id: CAS-318
title: >-
  Persistent Join and Set tables fail to insert on a CAS disk: tmp/<n>.bin
  parsed as a part file
status: To Do
assignee: []
created_date: '2026-09-26 22:02'
labels:
  - 'area:read-path'
  - 'area:write-path'
  - 'area:testing'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
  - 'needs:repro'
dependencies: []
priority: high
ordinal: 397000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Probe 2026-09-27 (docs/superpowers/reports/2026-09-27-cas-non-mergetree-engines-probe.md): INSERT into ENGINE = Join/Set with persistent = 1 and disk = 'cas' throws NOT_IMPLEMENTED 'Autocommit writes are not supported for content part files'. StorageSetOrJoinBase writes <table>/tmp/<n>.bin then replaceFile to <table>/<n>.bin (src/Storages/StorageSet.cpp:114,131,140); the CAS path parser takes the first component after the table uuid as the part component (Parts/PartPathParser.cpp:188-197), so tmp/1.bin is refused as a part file. The Log family (Log, TinyLog, StripeLog) works through the verbatim namespace-file mechanism and has no test either. Fix direction: only part-grammar names (and detached/, moving/) are part components; everything else under the table directory is a table-level path, nested or not. Same rule the table-files-as-refs design study assumes.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Persistent Join and Set on a CAS disk insert, read, survive a restart
- [ ] #2 Stateless test covering Log, TinyLog, StripeLog, Join and Set with disk = 'cas' (insert, restart, append, truncate, drop)
- [ ] #3 The part-component rule of the path parser is documented in docs/en/antalya/cas
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
