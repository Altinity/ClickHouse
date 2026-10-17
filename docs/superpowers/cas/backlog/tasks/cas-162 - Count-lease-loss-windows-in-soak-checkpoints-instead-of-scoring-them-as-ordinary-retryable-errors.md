---
id: CAS-162
title: >-
  Count lease-loss windows in soak checkpoints instead of scoring them as
  ordinary retryable errors
status: To Do
assignee: []
created_date: '2026-08-20'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:soak'
  - 'area:testing'
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/soak/cluster.py
  - 'https://github.com/Altinity/ClickHouse/issues/2243'
  - 'https://github.com/Altinity/ClickHouse/issues/2332'
priority: medium
type: task
ordinal: 208000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`soak/cluster.py` classifies "mount lease not held" (210) as retryable (`utils/ca-soak/soak/cluster.py:24-28`, `:199-218`),
so a soak rides through a #2243-style lease loss and scores it recoverable. Lease loss without a store outage is a live
theme (issues #2332, #2421, #2243); the harness should make it visible.
Add a checkpoint detector that counts `TransientNotLive` windows and mount-lease keeper errors, from `system.cas_log` mount
events or `metric_log`, and reports them in the scenario verdict.

Provenance: BACKLOG/mounts-and-lifecycle.md#issue-2243-port-exhaustion-lease (housekeeping: soak-harness blind spot); verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A soak checkpoint reports the number and total duration of lease-loss windows per node
- [ ] #2 A scenario that forces a lease loss shows a non-zero count in its verdict
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
First recorded: 2026-08-20 (d61e8ac938c, by 'issue-2243-port-exhaustion-lease')
<!-- SECTION:NOTES:END -->
