---
id: CAS-66
title: >-
  Apply the dynamic CAS disk settings on `SYSTEM RELOAD CONFIG` and warn about
  ignored ones
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:backend'
  - 'area:observability'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
  - src/Disks/DiskObjectStorage/DiskObjectStorage.cpp
documentation:
  - docs/en/antalya/cas/configuration.md
priority: medium
type: enhancement
ordinal: 80000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ContentAddressedSettings::loadFromConfig` runs only when the disk is created (`MetadataStorageFactory.cpp:233`). On reload
`DiskObjectStorage::applyNewSettings` forwards to `metadata_storage->applyNewSettings` (`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:984`),
and `ContentAddressedMetadataStorage` does not override the empty base (`IMetadataStorage.h:365`). An operator who edits `gc_interval_sec`,
cache budgets or any `gc_round_*` budget and reloads gets success, no log line and no change, while the S3 half of the same block does reload
(`DiskObjectStorage.cpp:988`). A typo added by edit-and-reload is not diagnosed until restart either.
Creation identities (`server_root_id`, `gc_shards`, `blob_hash`, `scratch_path`, `staging_backend`) must stay restart-only.
Shape: override `applyNewSettings`, re-parse and validate the block, apply the dynamic subset (GC cadence and budgets, cache budgets, `gc_enabled`),
and log a warning naming each changed creation-time key as ignored until restart. Matters for the pre-deployment defaults review (roadmap §7).

Provenance: BACKLOG/operability-and-introspection.md#cas-settings-not-reloadable-silently, reload half (2031-triage CAS-107); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Changing `gc_interval_sec` and a cache budget, then `SYSTEM RELOAD CONFIG`, changes behaviour without a restart, proven by a test
- [ ] #2 Changing a creation-time key logs a warning naming the key as ignored until restart
- [ ] #3 An unknown or invalid CAS key introduced by a reload is reported at reload time and the previous settings stay in force
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
First recorded: 2026-08-21 (f4acc4f955d, by 'cas-settings-not-reloadable-silently')
<!-- SECTION:NOTES:END -->
