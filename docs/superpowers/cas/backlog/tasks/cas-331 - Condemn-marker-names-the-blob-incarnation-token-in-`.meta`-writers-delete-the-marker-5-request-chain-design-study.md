---
id: CAS-331
title: >-
  Condemn marker names the blob incarnation: token in `.meta`, writers delete
  the marker, 5-request chain (design study)
status: Done
assignee: []
created_date: '2026-09-30 19:29'
updated_date: '2026-09-30 20:29'
labels:
  - cas
  - gc
dependencies:
  - CAS-30
priority: high
ordinal: 420000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Design study for the blob reclamation chain, to be analysed against the schedules of the CAS-30 spec r2 (S1, S2, S3, returning leader, `extdrop`) before it replaces or amends that spec.

Sketch (owner, 2026-09-30):
1. `HEAD blob` at condemn: observe the incarnation token `T`.
2. Conditional `PUT .meta` = `Condemned(round, size, token = T)`: the marker names the incarnation it guards. Blind create; a refusal is settled by the write loop's read; a marker naming another token is overwritten (this replaces the round re-stamp).
3. Writers delete the marker when they create or re-create the blob (instead of writing `Clean`); a marker that names the writer's own fresh token must not be deleted by the writer (S2).
4. `GET .meta` at graduation: present and `token == T` => graduate and keep the marker etag `M`; absent or another token => drop the entry from the condemned set (a writer re-created the blob or cleared the guard), no carry.
5. Last round: `DELETE blob If-Match T` and `DELETE .meta If-Match M`.

Five requests on the happy path. Open points to settle:
- `M` still lives in leader memory between graduation and delete: with the token in the marker the job can instead `GET` and check the token (+1 request, no memory, no orphan on leader change) - decide which.
- Step 4 "absent => drop the entry": today an absent marker means carry + rewrite (a swallowed marker write). With writers deleting markers, absent is ambiguous (swallowed write vs writer re-creation); the `HEAD blob` at step 5 with `If-Match T` still protects the body, so dropping the entry only delays reclamation until the next condemn. Check liveness: a blob whose marker write keeps failing must still get reclaimed eventually.
- Mixed versions: a marker without `token` (written by an older GC) guards the current body; older writers keep writing `Clean` instead of deleting - both must stay safe.
- `cas_blob_meta` is a Tolerant object: an optional plain `token` key is readable by 26.6.4; still a change to the frozen v1 format that the owner must approve.
- Model: `CaGcCondemnMarkerGate` variant with token-carrying markers and writer deletes; invariants `NoDangle`, marker-guards-named-incarnation, liveness.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written comparison against spec r2 (pinned delete) and the token-in-marker variant: requests per blob, state kept in memory, behaviour on leader change, format impact, model size
- [ ] #2 Every schedule of the r2 spec (S1, S2, S3, returning leader, extdrop) walked request by request under the sketch, with a verdict
- [ ] #3 Owner decision recorded: which variant CAS-30 PR1 implements
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
2026-09-30 study done: report docs/superpowers/reports/2026-09-30-cas-331-condemn-marker-variants.md. Owner decision: PR1 = minimal fix (G1 + r1 round checks + re-stamp); V1/V2r/V3 and all findings recorded in CAS-329 for the request-cut work.
<!-- SECTION:NOTES:END -->
