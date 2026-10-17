---
id: CAS-297
title: >-
  Review every CAS default before the first deployment and record the evidence
  for each
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:mounts'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-0
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
  - src/Storages/MergeTree/MergeTreeSettings.cpp
documentation:
  - docs/en/antalya/cas/configuration.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f14
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f25
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f27
priority: critical
type: task
ordinal: 376000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The roadmap lists "defaults reviewed" as a first-deployment blocker. There are 31 CAS disk settings (`CA/ContentAddressedSettings.cpp`)
plus MergeTree and S3 settings that CAS relies on.
Defaults with evidence against them already: `gc_round_ref_cleanup_budget` 5000 (the cap caused the `_log` growth loop, audit F14;
spec C2 / CAS-29.11 removes the budgets), `manifest_decode_cache_bytes` 128 MiB (full on the canary, F25), `part_folder_cache_bytes`
64 MiB (weight bug CAS-22), keep-alive (one new TLS connection per ~98 requests, F27; CAS-283, CAS-74),
`concurrent_part_removal_threshold_for_remote_disk` 16.
Lease and request timing: `mount_lease_ttl_ms` 30000, `mount_renew_period_ms` 10000, `attempt_timeout_ms` 5000,
`lease_safety_margin_ms` 2000. Concurrency: `gc_read_concurrency` 16, `gc_meta_pool_size` 16, `gc_shards` 1, `gc_interval_sec` 60.
Inputs from other tasks: the decode-cache and parallel-removal tasks of this unit, CAS-107 (sizing model), CAS-128 (unexposed knobs).

Provenance: umbrella-roadmap.md section 7 bullet 'Defaults review before the first deployment'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A table lists every CAS setting with its default, the evidence (soak, canary, or none) and a keep or change decision
- [ ] #2 Every changed default is applied, with the old value noted for `compatibility` where the setting is user-visible
- [ ] #3 `configuration.md` states for each setting when an operator should change it
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `mount_lease_ttl_ms` is 2026-07-02 (cacba14492b); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
