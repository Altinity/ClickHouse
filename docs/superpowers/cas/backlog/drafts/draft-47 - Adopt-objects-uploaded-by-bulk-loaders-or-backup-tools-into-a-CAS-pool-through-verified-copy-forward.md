---
id: DRAFT-47
title: >-
  Adopt objects uploaded by bulk loaders or backup tools into a CAS pool through
  verified copy-forward
status: Draft
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:write-path'
  - 'area:backend'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:spec'
dependencies: []
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Data written to the bucket by something other than this server (bulk load, backup tooling) would land under a staging prefix
and be adopted by hashing and publishing it (copy-forward), never trusted in place (hash equality needs the adversary model).
Distinct from the landed opt-in S3-native writer staging and from the condemned-evidence copy-forward spec
(`docs/superpowers/cas/history/2026-07-02-cas-copy-forward-condemned-evidence.md`). No spec exists.
Roadmap §1: "Adopting externally uploaded data — Later".

Provenance: BACKLOG/formats-and-storage.md [out-of-band staging adoption]; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec states the staging layout, the verification step and who may write to the staging prefix
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'out-of-band staging adoption')
<!-- SECTION:NOTES:END -->
