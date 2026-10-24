---
id: CAS-187
title: 'State what an equality-resolved conditional write proves, in types and docs'
status: To Do
assignee: []
created_date: '2026-08-20'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:backend'
  - 'area:docs'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2207'
  - R/Backend/CasRequests.cpp
  - R/Gc/CasGcMetaWriter.cpp
documentation:
  - docs/en/antalya/cas/architecture/backend.md
priority: low
type: chore
ordinal: 244000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Issue #2207 (2031-triage CAS-021) read an equality-resolved write as authorship. The integrity consequence is neutralized (delete-site in-degree re-read, exact-token deletion, fence check before publication), so this is honesty, not safety.
Done since: `slotOccupy` and its `NotUnresolved` label are gone; `Committed` carries `resolved_by_read` (`R/Backend/CasWriteResult.h:42`, set at `R/Backend/CasRequests.cpp:1069`).
Still open: the resolved arm returns the observed occupant's etag as if this call authored it; no trust-model doc block at the resolution ladder; no ownership-decidability table by key class (immutable content-addressed, mutable identity-in-payload, mutable identity-free, owner anchor `claimOwnerOrThrow`).
Also owed: cross-reference sentences at `writeCondemnedMeta` and `reconcileMetaClean`, pin tests renamed to read as spec, and one trust-model paragraph in `docs/en/antalya/cas/architecture/backend.md`.
Stale condemn-marker memo: accepted residual (user decision 2026-08-20). Rejected: a re-read fix, which costs one billable GET per graduating condemned entry to save free DELETEs in a rare race.
Sanctioned: label the self-heal as "spared by token rotation", zero extra requests; first re-check whether the `(ref, token)`-keyed memo (`R/Gc/CasGcMetaWriter.cpp:204-213`) still admits the race.

Provenance: BACKLOG/ref-protocol.md#cas-021-followups items (1), (2), (3). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792 (resolved_by_read present, slotOccupy absent on both).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No caller can read an equality-resolved outcome's etag as its own write without naming `resolved_by_read` (type-level split or accessor)
- [ ] #2 The resolution ladder carries the trust-model block and the decidability table; `backend.md` has the paragraph
- [ ] #3 The stale-memo self-heal is counted or logged as benign, or a note records that the memo keying closed the race
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
First recorded: 2026-08-20 (99031303637, by 'cas-021-followups')
<!-- SECTION:NOTES:END -->
