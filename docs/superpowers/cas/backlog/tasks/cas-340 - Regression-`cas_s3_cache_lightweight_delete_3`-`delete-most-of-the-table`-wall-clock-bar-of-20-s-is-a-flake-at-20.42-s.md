---
id: CAS-340
title: >-
  Regression `cas_s3_cache_lightweight_delete_3`: `delete most of the table`
  wall-clock bar of 20 s is a flake at 20.42 s
status: To Do
assignee: []
created_date: '2026-10-01 17:03'
labels:
  - ci
  - tests
dependencies: []
priority: low
ordinal: 433000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Observed 2026-10-01 on PR #2463 (release, x86): the scenario asserts execution_time < 20 and measured 20.42 s; the aarch64 run of the same shard and the same scenario on #2452 passed. A 2 % margin on a wall-clock bar on shared runners. Evidence: tmp/ci-2463/rca.md. Fix in clickhouse-regression: widen the bar or assert the ratio against a baseline measured in the same run.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The scenario no longer asserts an absolute wall-clock bound, or the bound has a margin that survives shared-runner noise
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
