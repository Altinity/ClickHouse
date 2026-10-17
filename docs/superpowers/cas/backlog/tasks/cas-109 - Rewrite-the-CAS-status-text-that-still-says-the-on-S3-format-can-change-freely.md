---
id: CAS-109
title: Rewrite the CAS status text that still says the on-S3 format can change freely
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-0
dependencies: []
references:
  - docs/en/antalya/cas/index.md
  - docs/en/antalya/cas/roadmap.md
priority: medium
type: docs
ordinal: 147000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
decision-4 froze the on-S3 format at `26.6.4.20001.altinityantalya`; later changes ship a new format version with a
compatibility path. The user docs still say the opposite: `docs/en/antalya/cas/index.md:77-80` ("Pre-release means the
format can change cheaply, with zero compatibility ..."), `docs/en/antalya/cas/roadmap.md:12` and `:88` ("the format and
settings surface can still change ... pre-release"). An operator planning a first deployment reads these pages first.

Provenance: BACKLOG/operability-and-introspection.md#b180-format-freeze (side note); verified 2026-09-26 against dd0ed2f263a. Related decision-4.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `index.md` and `roadmap.md` state the format is frozen as of `26.6.4.20001.altinityantalya` and that later versions keep a compatibility path
- [ ] #2 No page under `docs/en/antalya/cas/` says the on-S3 format can change without compatibility
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

Identifier trace: the earliest docs mention of `26.6.4.20001.altinityantalya` is 2026-09-16 (cbf3fe14e36); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
