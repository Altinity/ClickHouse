---
id: CAS-65
title: >-
  Reject unprefixed CAS setting names once external configs use the `cas_`
  spelling
status: To Do
assignee: []
created_date: '2026-08-25'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
  - src/Disks/tests/gtest_cas_settings.cpp
documentation:
  - docs/en/antalya/cas/configuration.md
  - docs/en/operations/storing-data.md
priority: low
type: chore
ordinal: 79000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Scheduled removal, not a defect. `ContentAddressedSettings::loadFromConfig` (`CA/ContentAddressedSettings.cpp:115-222`) still accepts the unprefixed
spelling, applies it and emits one aggregated `LOG_WARNING` (`:218-222`). While the window is open, a stale config runs with a warning nobody reads.
Trigger: the CAS configurations in the `clickhouse-regression` suite are on the `cas_` spelling (external; not checkable from this repo).
Work: replace the warning and apply loop with a throw listing every unprefixed name and its `cas_` spelling; rewrite
`LegacySpellingStillLoadsDuringMigrationWindow` and `PartialMigrationLoadsAndReportsEveryLegacyKey` (`src/Disks/tests/gtest_cas_settings.cpp:248`, `:256`)
into one test that pins the rejection; drop the deprecation text from `docs/en/antalya/cas/configuration.md` (section `#migration-from-unprefixed-keys`)
and `docs/en/operations/storing-data.md`. Keep this separate from the spec C2 rule that removed `cas_gc_round_*` keys stay accepted as no-ops for one release.

Provenance: BACKLOG/operability-and-introspection.md#cas-config-prefix-window (landed with 917600b122b); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Blocked on the external clickhouse-regression migration.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A config with an unprefixed CAS setting fails disk creation with an error naming the key and its `cas_` spelling
- [ ] #2 The migration tests are replaced by one test pinning the rejection, and the deprecation text is gone from both docs
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
First recorded: 2026-08-25 (c2fd57cb32a, by 'cas-config-prefix-window')
<!-- SECTION:NOTES:END -->
