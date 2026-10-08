---
id: CAS-154
title: >-
  Derive the CAS attempt timeout from the lease budget and clamp the connect cap
  instead of refusing the mount
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
milestone: m-0
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.cpp
  - tests/integration/test_cas_s3/configs
  - 'https://github.com/Altinity/ClickHouse/pull/2300'
documentation:
  - docs/en/antalya/cas/configuration.md
priority: medium
type: enhancement
ordinal: 197000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The operator sets TTL, renew period, `attempt_timeout_ms` (default 5000, at least 1: `CA/ContentAddressedSettings.cpp:84`,
`:245-248`) and margin; the connect cap is `min(connect_timeout_ms, attempt)`. The mount refuses when
`period + 2 x (attempt + 2 x cap) + margin >= TTL`, so a disk with `connect_timeout_ms >= 2 s` under the old defaults cannot mount.
Found in the PR #2300 run-3 triage; interim fix pins `connect_timeout_ms` 1000 in the `test_cas_s3` config.
Proposal: `attempt_timeout_ms = 0` means auto = `(TTL - period - margin)/5 - 2 x cap` (five attempts fit the renewal window);
clamp the connect cap to the lease budget with one Warning at mount; show the effective cap in `system.cas_mounts` and the
"budget in effect" log line; document the fencing latency (observation = TTL + TTL/20 + period/2; GC fence-out =
TTL + TTL/20 + period).

Provenance: BACKLOG/mounts-and-lifecycle.md#mount-lease-budget-derived-timeout; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A disk with `connect_timeout_ms` of 2 s or more mounts with a clamped cap and one Warning
- [ ] #2 `attempt_timeout_ms = 0` selects the derived value, and the effective value is visible in `system.cas_mounts`
- [ ] #3 The configuration docs state both fencing-latency formulas
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
First recorded: 2026-09-26 (43b3a043935, by 'mount-lease-budget-derived-timeout')
<!-- SECTION:NOTES:END -->
