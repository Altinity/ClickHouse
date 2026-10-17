---
id: CAS-247
title: >-
  Fix the public CAS docs: wrong `ca-*` command names and a correctness page
  citing a dev-only path
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - docs/en/antalya/cas/architecture/correctness.md
  - docs/en/antalya/cas/roadmap.md
priority: medium
type: docs
ordinal: 312000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
- 12c: 15 occurrences of unregistered `ca-*` names on 7 pages under `docs/en/antalya/cas/`: `roadmap.md:51` (x4),
  `architecture/garbage-collection.md:247,257,258`, `architecture/correctness.md:51` (x2), `architecture/read-path.md:81` (x2),
  `architecture/manifests-and-refs.md:125,276`, `architecture/blob-protocol.md:189`, `architecture/replication.md:123`.
  The registered commands are `cas-fsck`, `cas-inspect`, `cas-gc-dryrun`, `cas-gc-rebuild`, `cas-drop-member`.
- 12a: `architecture/correctness.md:21,25` cite `docs/superpowers/models/`, a development-branch path, as the evidence for
  CAS safety. Cite only what ships, or ship a public subset.
CAS-109 also edits `roadmap.md`; CAS-60 documents the `cas-*` commands in `clickhouse-disks.md`.

Provenance: BACKLOG/docs-and-cleanup.md#public-docs-accuracy-m12 (umbrella review M12) 12a and 12c; 12b dropped as done (a923b9888b6). Count re-derived (15, not 10). Verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No page under `docs/en/antalya/cas/` names a `ca-` prefixed `clickhouse-disks` command
- [ ] #2 `correctness.md` cites no path outside the shipped docs
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
First recorded: 2026-08-21 (3c7cd13190e, by 'public-docs-accuracy-m12')
<!-- SECTION:NOTES:END -->
