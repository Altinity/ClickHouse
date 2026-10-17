---
id: CAS-328
title: >-
  Remove `KeyStrictness`: every CAS reader skips unknown keys; decision-changing
  fields use the `!` prefix
status: In Progress
assignee: []
created_date: '2026-09-30 18:02'
updated_date: '2026-09-30 18:41'
labels:
  - cas
  - gc
  - format
dependencies: []
priority: medium
ordinal: 417000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Four readers are `KeyStrictness::Strict` and turn any unknown key into `CORRUPTED_DATA`: `cas_run` rows (`CasRecordStreamFormat.cpp:253`), fold seal (`CasFoldSealFormat.cpp`), `cas_ref_ckpt` (`CasRefCkptFormat.cpp:138`), ref catalog (`CasRefCatalogFormat.cpp`). Every other object is `Tolerant`. Strict readers make every optional field addition a compatibility-version bump (`G_BUILD`), which needs the not-yet-implemented write-down floor; the shipped 26.6.4 readers stay strict forever, so the payoff starts with the first release that carries this change.

The only stated motivation is the `_ckpt` comment: a field that changes what cleanup may delete must not be silently ignored. The evolution rule already covers that: such a field is written with the `!` prefix and an old reader fails with `UNKNOWN_FORMAT_VERSION`. Strict duplicates that rule as a guard against a writer forgetting `!`.

Owner decision 2026-09-30: all objects Tolerant unless a very important reason exists; none was found.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A `!`-prefixed unknown key still fails with `UNKNOWN_FORMAT_VERSION` in every reader (test per object)
- [ ] #2 The format spec states the rule: optional field = plain key, decision-changing field = `!` key; Strict is not used
- [ ] #3 Own PR to antalya-26.6, reader-only change, no writer change, no format version bump
- [ ] #4 The `KeyStrictness` enum, the traits-table column and the `JsonObjectReader` parameter are gone; `skipUnknown` keeps only the `!` branch (`UNKNOWN_FORMAT_VERSION`) and the skip
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
2026-09-30 PR opened: https://github.com/Altinity/ClickHouse/pull/2455 (base antalya-26.6, head fix/antalya-26.6/cas-drop-key-strictness-pr = a61b9a1a46b, 30 files +285/-199). Five Strict readers, not four (cas_gc_maintenance_state); seven own-throw sites routed through skipUnknown. Local gate CAS* 2564/0; review APPROVE, 4 minors closed. No local ASan run (no ASan build on disk); CI ASan lane covers it. Next: CI on the PR.
<!-- SECTION:NOTES:END -->
