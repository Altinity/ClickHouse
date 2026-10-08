---
id: CAS-245
title: Document that the bucket credential is the whole trust boundary of a CAS pool
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-0
dependencies: []
references:
  - docs/en/antalya/cas/bucket-requirements.md
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
documentation:
  - docs/en/antalya/cas/bucket-requirements.md
priority: medium
type: docs
ordinal: 310000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Nothing under `docs/en/antalya/cas/` states the trust model (grep for "trust boundary": 0 hits). The protocol authenticates no
writer of a control object (`CA/Pool/CasServerRoot.cpp`): every holder of the pool credential is trusted exactly as much as
every pool member and can retire a member, fence writes or claim a mount slot. Operators need three rules: never share the
prefix with an untrusted role; give backup, log-shipping and analytics roles read-only credentials; treat the credential
like cluster admin access.

Provenance: BACKLOG/docs-and-cleanup.md#pool-trust-boundary-undocumented (2031-triage CAS-027); verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `bucket-requirements.md` or `index.md` has a trust-boundary section with the three rules
- [ ] #2 The section lists the destructive actions a credential holder can take
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
First recorded: 2026-08-21 (47c403ae0ab, by 'pool-trust-boundary-undocumented')
<!-- SECTION:NOTES:END -->
