---
id: CAS-31
title: >-
  Stop one clamped or damaged namespace from suppressing every GC delete
  pool-wide indefinitely
status: To Do
assignee: []
created_date: '2026-07-03'
updated_date: '2026-09-26 14:44'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGc.h
  - src/Disks/tests/gtest_cas_gc_hold_grammar.cpp
priority: high
type: design
ordinal: 38000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`suppress_destructive = anomalies ∨ carried_holds ∨ frontier_incomplete` (`CA/Gc/CasGc.cpp:3203-3204`) and the suppression
is pool-wide by design, because blob in-degree is a pool-wide property (`CA/Gc/CasGc.h:62-64`). `e337bb2c87dc` made an
undecodable `_ckpt` hold only its own namespace, but that hold still closes every destructive site. A long persistent clamp
or one damaged object therefore stops all reclamation until an operator repairs it; bytes grow without bound. Safety is
fail-closed and correct; liveness is unaddressed. The 2026-07-18 S38 starvation shape is moot since `d74c726ef9e3`.
Needs a design: e.g. which delete families are provably independent of the held namespace's edges, or an operator-visible
alarm plus bounded degradation. Narrowing must keep invariant 1 (no delete of a still-named object).

Provenance: BACKLOG/gc.md [clamp liveness] and #ckpt-damage-no-repair-path part (a); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A design note states which delete families, if any, may proceed while one namespace is held, with the safety argument reviewed
- [ ] #2 An operator can see from `system.cas_gc_log` or a metric which namespace has blocked destructive GC and for how many rounds
- [ ] #3 A test with one held namespace shows the agreed behavior for every destructive family
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
First recorded: 2026-07-03 (9a72dc465aa, by 'clamp liveness')

Priority raised to High (u19c review, 2026-09-26): the soak measured a complete, indefinite pool-wide GC halt (4.6 h with zero deletes, pool at 112 GB, 764k unreachable objects, manual SYSTEM CAS GC REBUILD FORCE the only unwedge), which is the charter's measured-cost criterion, not a percentage overhead.

Merged from u19c-soak (Stop one clamped or damaged namespace from suppressing every GC delete pool-wide indefinitely): Soak measurement of the liveness cost (2026-07-10, seed 991, 4.6 h exit soak): one dropped table's namespace with 63 manifests
whose committed bodies were missing clamped the fold on every pass from t+540 s, 56.8k clamp events; the leader ran 800+
Success rounds with objects_deleted=0, the pool grew to 112 GB and 764k unreachable objects, and the in-run fsck timeouts
followed from the never-shrinking pool. Integrity held (dangling=0).
The cause of that shape is fixed (`c1485f52a29`), but a permanently missing committed body still clamps. Recovering the -1 set
from the snapshot was rejected: `sourceEdgeId` is a one-way CityHash128 of (ManifestId, path) (`CA/Gc/CasBlobInDegree.cpp:164`),
so a missing body's edges cannot be enumerated. The known unwedge is `SYSTEM CAS GC REBUILD FORCE` (rebuilds from root refs;
refuses only on a live ref naming a missing body).
Provenance: utils/ca-soak/scenarios/BACKLOG.md#GC-WEDGE-REMOVAL-FOLD-2026-07-10. Forensics (all 63 refs, shards, cursors): `utils/ca-soak/scenarios/gc_wedge_forensics_20260710.txt` at 97589860bc4, deleted in 13723e501a3.
<!-- SECTION:NOTES:END -->
