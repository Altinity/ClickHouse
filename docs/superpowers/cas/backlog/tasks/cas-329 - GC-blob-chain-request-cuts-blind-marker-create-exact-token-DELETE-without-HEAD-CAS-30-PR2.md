---
id: CAS-329
title: >-
  GC blob chain request cuts: blind marker create, exact-token DELETE without
  HEAD (CAS-30 PR2)
status: To Do
assignee: []
created_date: '2026-09-30 18:58'
updated_date: '2026-09-30 20:29'
labels:
  - cas
  - gc
dependencies:
  - CAS-30
priority: medium
ordinal: 418000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two of the 7 requests per reclaimed blob exist only for the form of the code, not for safety, and go away as PR2 of the CAS-30 series (spec `docs/superpowers/specs/2026-09-30-cas-gc-meta-delete-names-its-condemnation-design.md`, r2):

- Request 2, `GET .meta` before the condemn marker write: replace `writeCondemnedMeta`'s read-then-write by `op.create` (If-None-Match: *; the write loop already settles a refusal with a body read): committed -> done; refused and observed Condemned -> done; refused and observed Clean -> `replace` with the observed etag. Common case (no marker) 1 request instead of 2.
- Request 5, `HEAD blob` before the exact-token delete (`performRedeleteIo`): reconstruct the live `Etag` from `PersistedEtag{dialect, value}` + the current backend id + key and issue `op.remove(key, etag)` directly; the backend already classifies Removed / TokenMismatch (=> Replaced) / NotFound (=> Absent). Check the round trip for every dialect (`etag`, `generation`, `emulated`).

Each is its own commit; A/B on the gc-ab stand (`utils/ca-soak/gc-ab`) in a GC-only window: HEAD and GET per reclaimed blob, and round time with 20 ms latency.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Per reclaimed blob on the happy path: 1 HEAD, 1 GET (graduation), 1 PUT, 2 DELETE — measured on the gc-ab stand
- [ ] #2 No fallback path: a refused create is settled by the write loop's own read; an unknown token form is a LOGICAL_ERROR, not a HEAD
- [ ] #3 TLA model `CaGcCondemnMarkerGate` updated: the blind create and the direct conditional delete keep every invariant of the r2 spec
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
2026-09-30 OWNER DECISION: CAS-30 PR1 stays the minimal fix - G1 (remove the in-process confirmation registry; graduation always reads the marker: the registry was a request-saving optimization that is being reverted) plus the round checks of spec r1 (C1 job, C3 gate) and the re-stamp (C2). No pins, no leader memory, no format change, 8 requests per reclaimed blob. Everything below is the research record for the request-cut work and its design alternatives.

Variants studied (docs/superpowers/reports/2026-09-30-cas-331-condemn-marker-variants.md; model, cfgs and traces in tmp/cas331/; spec r2 = V1 at docs/superpowers/specs/2026-09-30-cas-gc-meta-delete-names-its-condemnation-design.md; codex review of r2 in tmp/cas30/spec_review_r2.log):
- V1 (spec r2): graduation keeps the marker etag M in leader memory, delete job `DELETE If-Match M` without a read (-1 GET); pins only from committed attempts; unknown M => skip. Residuals: rebuild/round-regression corner = data loss on every backend (codex MAJOR 2); orphan `.meta` on every leader change (PR3 = CAS-330 would persist M in the strict `cas_run` row, needs CAS-328 everywhere); `extdrop` not closable (W1 unsound under equal-round byte ABA on S3). codex r2 MAJOR 1 applies to any always-read variant: the gate runs before `graduationAvailable` in `CasBlobInDegree.cpp` (~:493-498), so the budget must be checked before the read or a carried backlog is re-read every round (N, N-B, ...).
- V2 (token in marker, token-only acceptance, overwrite markers naming another token): LOSS - traces P1 (`v2u_min_nd`: a stalled condemn write for a dead token replaces the pending guard, its stale job deletes it) and P1b (`v2o_min`, `v2r_nogateround_nd`: a low-round marker accepted on its token is replaced by a stale higher-round write). Rounds order GC writes; the token names the incarnation; both are needed.
- V2r (recommended by the study): marker `Condemned(round, size, token)` as an optional plain key of `cas_blob_meta` (Tolerant; 26.6.4 skips it via `skipUnknown`; header stays v:1; tokenless encoding byte-identical), r2's write rule and re-stamp, gate = Condemned AND token == T AND round >= c, delete job GETs and deletes only a marker naming its own dead token. Green: `v2r_min` 5.57M states, all invariants; `extdrop` closed by W1 (`v2r_w1_extdrop_r6` green at 53.4M states, no violation to ~119M at MaxRounds 7). No leader memory, no orphan, rebuild corner becomes a bounded leak. Cost: 8 requests (7 after the HEAD cut), i.e. +1 GET vs V1; format extension needs an owner rule ("optional plain keys in Tolerant objects are compatible v1"). src estimate -45/+42 vs V1 -45/+77. Model 23x `pm_g1`.
- V3 (owner sketch: writers delete the marker, "absent => drop"): LOSS via P1/P1b as sketched; "absent => drop" leaks a body per swallowed marker write; changes the writer path on every publication (+120 src). If revisited: V2r rules + writers deleting markers as its own measured task.
- Side finding: writers leave a `Clean` marker on every publication (`PartWriteTxn::ensureBlobPresent` backfill/reconcile), so the blind marker create (request 2 cut) usually costs 3 requests, not 1-2; the realistic floor is 6 per reclaimed blob, not 5.
- Failed predictions recorded in the study: the gate's round check is load-bearing (`v2o_min`); the gate's token check is not shown to be (`v2r_notokcheck_nd` green) - it is load-bearing in the delete job.

Request-chain reference (base e2dd0f5d343): 1 HEAD blob at condemn; 2 GET .meta; 3 conditional PUT .meta; 4 GET .meta at graduation (unconditional after PR1); 5 HEAD blob before delete; 6 DELETE blob If-Match; 7 GET .meta; 8 DELETE .meta If-Match. Safety core: 1, 3, 4, 6; form-only: 2 (blind create - see side finding), 5 (persisted token reconstructable into `Etag{backend_id,key,dialect,value}`); hygiene: 7-8.
<!-- SECTION:NOTES:END -->
