---
id: CAS-117
title: >-
  Specify how a pool moves to a new on-S3 format version across mixed-version
  servers
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:formats'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'touches:settings'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: medium
type: design
ordinal: 155000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
decision-4: every format change after `26.6.4.20001.altinityantalya` ships a new version with a compatibility path and a
documented upgrade order. Readers already fail closed on an unknown version (`UNKNOWN_FORMAT_VERSION`,
`CA/Formats/CasFormat.h:70`, `CasPartManifestFormat.h:100`), so a server that writes a newer format before every pool member
reads it breaks the others. Missing: a durable pool roster of member versions and a `max_content_addressable_pool_format`
(or equivalent) setting that holds writers at the old version until all readers are upgraded. Neither exists on either
branch; no rollout spec exists in `docs/superpowers/specs/`. Needed before the first post-freeze format change.

Provenance: BACKLOG/operability-and-introspection.md#b180-format-freeze (rollout half, [B180]) and #b13-migration-path (mixed-version rule); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792. Related decision-4.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec defines the rule (read new before write new), where the roster lives, and how writers learn the allowed version
- [ ] #2 The spec gives the documented upgrade and downgrade order for a two-version pool
- [ ] #3 The spec is reviewed and accepted before any format v2 change is implemented
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'B180 / format-freeze')
<!-- SECTION:NOTES:END -->
