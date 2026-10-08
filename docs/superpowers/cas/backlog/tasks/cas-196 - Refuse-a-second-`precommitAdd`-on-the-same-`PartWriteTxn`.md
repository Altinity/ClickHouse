---
id: CAS-196
title: Refuse a second `precommitAdd` on the same `PartWriteTxn`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - R/Pool/CasPartWriteTxn.cpp
  - R/Pool/CasPartWriteTxn.h
priority: low
type: bug
ordinal: 253000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`PartWriteTxn` holds one precommit binding (`precommit_target_ns`, `precommit_final_ref`, `precommit_manifest`, `R/Pool/CasPartWriteTxn.h:378-380`), and every consumer assumes one.
`precommitAdd` (`R/Pool/CasPartWriteTxn.cpp:702`) overwrites it on every call (`:742-745`); a second call would orphan the first binding. No production path calls it twice today.
Also decide: the idempotent re-add arm still ends `Durable` (`:801`), so a later `abandon` appends a removal for a binding this build never owned and fails loudly.
Fix: throw `LOGICAL_ERROR` when `precommit_state != NotAttempted`, or hold a container all three consumers iterate.

Provenance: BACKLOG/ref-protocol.md#precommit-add-single-slot-guard (2031-triage CAS-072). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A second `precommitAdd` is refused; its test is a death test under `DEBUG_OR_SANITIZER_BUILD`, where `LOGICAL_ERROR` aborts
- [ ] #2 The idempotent re-add arm leaves `abandon` a no-op for a binding it never owned (gtest)
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
First recorded: 2026-08-21 (2a29781641d, by 'precommit-add-single-slot-guard')
<!-- SECTION:NOTES:END -->
