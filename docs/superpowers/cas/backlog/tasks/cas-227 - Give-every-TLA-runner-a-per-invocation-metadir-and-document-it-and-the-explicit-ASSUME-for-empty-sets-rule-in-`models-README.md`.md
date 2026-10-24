---
id: CAS-227
title: >-
  Give every TLA runner a per-invocation metadir and document it and the
  explicit-ASSUME-for-empty-sets rule in `models/README.md`
status: To Do
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - docs/superpowers/models/run_tlc.sh
  - docs/superpowers/models/run_b140danglemerge.sh
  - docs/superpowers/models/README.md
documentation:
  - docs/superpowers/models/2026-07-30-empty-set-survey.md
priority: low
type: chore
ordinal: 285000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two overlapping TLC runs sharing a metadir corrupt each other: `CaB140DangleMerge`'s `m_merged` config went red that way and passed on a clean
metadir (20692441 states, 9 s). `run_tlc.sh:74` and `run_ebo.sh:40` now derive a unique `run_id`, but about 18 runners still build a static path,
including the incident's own `run_b140danglemerge.sh:29`, `run_gclease.sh:27`, `run_mount.sh:158`, `run_refcatalog.sh:96`.
`models/README.md` states neither this convention nor the explicit-`ASSUME` rule for empty entity sets that `CaBuildRootPrecommit` follows.

Provenance: BACKLOG/testing-and-ci.md#tla-runner-static-metadir (source's 'mostly fixed' is wrong) and #empty-set-survey-residues (README convention); verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every `docs/superpowers/models/run_*.sh` uses a per-invocation metadir, preferably from one shared helper
- [ ] #2 `models/README.md` states the metadir rule and the explicit-`ASSUME` rule for empty entity sets
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
First recorded: 2026-07-30 (d0d73d46835, by 'empty-set-survey-residues')
<!-- SECTION:NOTES:END -->
