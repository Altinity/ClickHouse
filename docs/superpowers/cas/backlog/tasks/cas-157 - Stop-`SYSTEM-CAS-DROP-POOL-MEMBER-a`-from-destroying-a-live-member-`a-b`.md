---
id: CAS-157
title: Stop `SYSTEM CAS DROP POOL MEMBER 'a'` from destroying a live member `a/b`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:medium'
  - 'origin:2031-triage'
  - 'needs:decision'
  - 'confidence:plausible'
  - 'needs:repro'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h
  - src/Disks/tests/gtest_cas_decommission.cpp
priority: medium
type: bug
ordinal: 203000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`validateServerRootId` accepts slashes (`CA/Pool/CasServerRoot.h:86`; multi-segment ids were deliberately enabled by
`b97847d32f9`), but decommission selects victims by raw path prefix: namespaces by `victim_srid + "/"`
(`CA/Tools/CasDecommission.cpp:204-209`) and staging by `staging/<srid>/` (`:295`). So decommissioning `a` removes the
namespaces and control objects of a live member `a/b`. Mounting `a` over an existing `a/b` already fails closed (the subtree is
not empty); the reverse order is unguarded. Relink routing compares srid exactly, so the prefix rule is decommission-local.
Existing test covers only the sibling case `victim`/`victim2`.
Options: exact-srid selection plus a refusal when another member's srid is prefixed by the victim's; or forbid nesting at
validation (cheaper, removes the multi-segment layouts).

Provenance: BACKLOG/mounts-and-lifecycle.md#nested-srid-decommission (2031-triage CAS-007). Found by code reading, not observed, so priority is capped at Medium. Verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Decommissioning `a` while `a/b` is a member either refuses or leaves every object of `a/b` intact
- [ ] #2 `gtest_cas_decommission.cpp` has a nested-srid case that fails on the current code
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
First recorded: 2026-08-21 (69e007cf41e, by 'nested-srid-decommission')
<!-- SECTION:NOTES:END -->
