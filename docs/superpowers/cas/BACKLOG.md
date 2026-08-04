---
description: 'Consolidated live backlog of all still-pending CAS MergeTree work items. Single source of truth for what is left; issue IDs preserved (never renumbered). Groomed 2026-07-13, re-groomed 2026-08-04.'
sidebar_label: 'CAS Backlog (live)'
sidebar_position: 9
slug: /superpowers/cas/backlog
title: 'CAS MergeTree — Live Backlog (pending issues)'
doc_type: 'guide'
---

# CAS MergeTree — Live Backlog {#cas-backlog}

This is the **single canonical list of everything still pending** for the content-addressed (CAS)
MergeTree feature. It was groomed on **2026-07-13** by consolidating the roadmap, the per-effort
specs/plans/worklogs/reports, the whole-branch review (`review1.md`), and the refactoring/review TODO
notes, then verifying each candidate against the code at HEAD (branch `cas-gc-rebuild`). Issue IDs
(B-numbers, T-numbers, S-numbers, D-numbers, review IDs, rev.6, …) are **preserved, never renumbered**.

**Re-groomed 2026-08-04, then aggressively pruned the same day** against the 2026-08 docs-consolidation
verdict set (`docs/superpowers/cas/consolidation-2026-08/verdicts/verdicts.jsonl` joined against
`clusters/clusters.jsonl` both by `issue_ids` and by `sources` citing this file's own anchors).
Every item whose resolution was individually content-verified — not just verdict-matched — was
either deleted outright (fully resolved; git history is the archive) or, where a long investigation's
diagnosis was confirmed accurate but its recommended fix was still open, compressed to the live ask
plus an evidence pointer, dropping the resolved narrative. Items with any still-open thread stayed
live, and items with no cluster match were left untouched — the consolidation corpus does not claim
full coverage of this file, and absence of a match is not evidence an item is resolved. This is a live
document again, not an archive; git history holds everything trimmed.

`ROADMAP.md` remains the DONE/history status roll-up; **this document is the live backlog.** For each
item: `[<ID>] title — priority — one-line status/pointer`.

Priority legend: **GATE** = release gate; **HARD** = agreed-necessary, not yet done; **DESIRABLE** =
valuable, not committed; **DOC** = documentation debt; **TEST/INFRA** = validation/harness/CI;
**MINOR** = small concrete improvement; **VERIFY** = believed open, confirm before working.

> **What just closed (2026-07-13 grooming).** The following large efforts are **DONE at HEAD** and are
> NOT in this backlog: ref snapshot+log lifecycle (Phase 1), mixed-algo pools (Phase 3), pluggable
> blob hash **incl. Phase 2 sha256**, part-folder cache (phases 1–5), S3-native staging (opt-in),
> file-cache-disk-over-CA, GCS generation binding (validation-grade), retired-in-snapshot, add-only GC
> freshness meta / deposed-leader `clearSparedMeta` fix, writer↔GC simplification Phase A + freshness-v3
> (Phase B), promote-over-committed / promote-resurrect / tokenless copy-forward, introspection package
> (`system.content_addressed_mounts` + mount audit events + gauges), GC round skip-unchanged Lever A,
> GC snapshot streaming T2/T0, and the **entire 2026-07-12 stabilization & cleanup iteration** (A1–A10,
> B1–B4, C1–C5, D1–D5, E1–E3, F1, R412, RExpect — every task committed). B31 capability gate, B192
> event-name review, and several B10 minor findings (`~Build` noexcept, `inDegreeInGeneration`,
> redundant watermark GET, signed-in-degree accumulation) also verified closed. See §Recently closed.


> **What just closed (2026-07-26, the publish-confirm + introspection round).** DONE at HEAD and not
> carried forward: **Part A** — ref-lane exception safety, tasks 1-8 (uncertain-`precommitAdd` intent
> recorded before the append, `PreparedPartWrite` move-only handle, `confirmExactRef` with the try-lock
> that removed a pool-wide append stall). **Part B** — the fetch-handoff publish-confirm protocol,
> tasks 9-16, protocol v11, plus its codex review and the four findings that review produced
> (`8e6fe6ef0af`; two of the reviewer's four remedies were WRONG and were not applied — see
> {#partb-review-resolved}). **Introspection I1-I4** — `stale_edge` fsck class, unmatched-remove-delta
> counter, DEFER rounds no longer logging as "round 0", `ca-inspect` decoding source-edge runs.
> **`CasRefAppendPreAttemptRefused`** counter. **The harness anti-vacuity sweep** — every soak assert
> can now go red, and signal reads are separated from zeros. **Per-phase GC log rows** (Q5 option (c),
> 18 phases/round). **The skipped-transaction detector** (option C, probes A/B1/B2). **The CI marker-based
> CA-disk fix.** **The third gate-filter gap.** **Soak gate** — 3x 20-min green plus a 4h chaos run:
> 0 failures, 40 checkpoints, 529 signal reads, 72 real `stale_edge` evaluations, 1,673 round attempts.
>
> **What did NOT close and is the material of the next round** (GC performance + blobs that never get
> reclaimed): the LIST-as-journal deletion hole {#list-as-journal-dataloss-2026-07-25} (release blocker,
> mechanised in TLA+, detector built but the defect not yet caught by it); the unmatched-minus-one
> retention leak {#unmatched-minus-one-retention-leak} (ROOT-CAUSED, fix NOT landed); the GC bottleneck
> study {#gc-bottleneck-study-2026-07-25} (deliverable 1 shipped; the rig and the controlled measurements
> are not built); probe A's 14 firings (next step named: lease identity at each end of the enumeration);
> S42 at scale; the four instrumentation-review recommendations; `ca-fsck` never printing `corrupted_runs`;
> the fsck 180 s flat budget; force-claim tasks 10-12; the residual `eraseView` post-commit window.

---

## NEXT ROUND (opened 2026-07-26): GC performance, and blobs that never get reclaimed {#round-gc-perf-and-stuck-blobs}

Two themes, chosen by the user. They are listed together because the evidence says they are the same
system seen from two ends: a GC that cannot keep up leaves work undone, and work left undone is what an
operator sees as blobs that never go away.

**A terminology note, stated because it matters for what we go looking for.** In `ca-fsck` vocabulary
`dangling` means *referenced but MISSING* — that is the data-loss class, and it has been **zero in every
run to date**. The class that actually gets stuck is `unreachable`/`awaiting-gc`: present, unreferenced,
and never reclaimed. Where this round says "stuck blobs" it means the second. If a true `dangling` ever
appears, that is not a performance topic — it is INV-NO-LOSS and it stops everything.

### Known, root-caused, not yet fixed {#round-gc-known}

- **The unmatched-minus-one retention leak** — {#unmatched-minus-one-retention-leak}. 56 blobs, each
  holding exactly one residual source edge whose manifest no longer exists, all traced to 4 `tmp-fetch_*`
  refs published and dropped inside a 43 ms window (4 of 48,791 such refs). Root cause is measured, not
  inferred. The fix is NOT landed, and the obvious remedy — queue the exact removal — is WRONG:
  `RefTableState` throws `CORRUPTED_DATA` on an absent binding, which would convert a leak into a
  permanent wedge. A correct fix has to reconcile the in-degree, not re-issue the delete.
- **RESOLVED 2026-07-30: `ca-fsck` never printed `corrupted_runs`** — {#fsck-corrupted-runs-invisible}. Now on
  the summary line, in the nonzero-exit set, and a row of `kFsckHardFindings`.
- **The fsck 180 s flat budget** — times out once the ref space is large, so the check that would find
  stuck blobs is the check that stops running exactly when the pool gets big enough to have them.

### Performance: what is measured, and the two open questions {#round-gc-perf}

Measured so far, from 1,673 rounds of phase data — {#gc-perf-first-measurement} and its same-day
correction {#gc-perf-gets-per-log}:

- `fold_ref_intake` dominates and is LINEAR in the ref-log backlog: **256 logs/s** sustained.
- The flat cost is **per REQUEST (~0.92 ms)**, not per log. Requests per log run **2.5 to 4.7 and climb
  with backlog size** — a multiplier of about four that nobody has examined.
- `orphan_sweep` has the worst MEDIAN (383 ms/round) — a constant tax, a different shape from intake's
  five-orders-of-magnitude tail.
- `lease` is free (2 ms p50 across 2,255 rounds); nobody should optimise it.

Open question 1 — **where does the 4x multiplier come from?** `foldManifestEdges` GETs a manifest body
per edge and the same body can be re-read across logs within a round with no cache between.
`CasRefManifestBodyFoldGets` is already on every phase row, so this is a QUERY, not an experiment.

Open question 2 — **is 256 logs/s enough, and for what arrival rate?** The CI collapse was a queue
crossing: service rate below arrival rate, each round longer than the last. That needs the reproduction
rig ({#gc-bottleneck-study-2026-07-25} deliverable 2), which does not exist, plus a chaos-free run so the
tails are work rather than frozen processes.

### The release blocker that rides along {#round-gc-blocker}

{#list-as-journal-dataloss-2026-07-25} — GC advances its fold cursor over records a paginated LIST merely
OBSERVED, with no completeness proof. Mechanised in TLA+. The detector shipped this round (probe A) and
has FIRED 14 times, but has not yet been shown to be catching the real thing rather than concurrent
deletion between the two enumerations. Next step is named and small: record the lease identity at each end
of the enumeration. This is a GC-correctness item, not a performance one, but it lives in the same code
and the same round.

### Do NOT start with {#round-gc-not-first}

Force-claim (follow-ups tasks 10-12) — its CI motivation evaporated when the one-line CI fix landed
({#operator-uuid-recovery}). Stage 2 concurrent `commitPart` — postponed by user decision. Neither is in
this round unless something new argues for it.

---

## 1. Ref protocol — rev.6 lease-boundary exclusivity (highest-priority open design) {#ref-protocol}

- **[rev.6] Lease-boundary exclusivity — remove the grace window + publish-path replay** — HARD (user-driven) — Proposal `specs/2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md`, **LANDED** (15-commit rev.6 plan complete, soak-gated green). Solved writer exclusivity once at the mount-lease handover: unclean-handover wait (`materialization_grace_ms`/`T_mat`), `released_clean` clean-unmount fast path, eager recovery-snapshot **seal** (mount writable only after it commits), publish-from-live snapshots. Deletes `snapshot_min_log_age_ms` + the per-entry `RefTableState` replay. Amendment checklist 1–7 + open questions Q1–Q4 (`T_mat=30s` acceptable? eager snapshot on clean large-tail mounts? hard-require wedge/single-in-flight at the lane?). Interim mechanical patch already landed (`3c7003ce190`: aged+uncovered trigger, copy-once replay, threshold 64→256). SUPERSEDED CODA (2026-07-29): `materialization_grace_ms`/`T_mat` itself was later retired OUTRIGHT by Stage A T12 (`ff9f36a056f`) — the v9 in-band `EpochSeal` owns cross-epoch exclusivity now; see `2026-07-28-stage-a-retirement-verdicts.md`.
- **[Late Predecessor PUT] cross-epoch late-materialization correctness limitation** — HARD — The hazard rev.6 closes: a fenced predecessor's in-flight PUT can materialize below successor snapshot coverage (a missed `−1`/`+1` = data-loss class). Phase-1 documents it; the fix LANDED as the v9 in-band `EpochSeal` (INV-2, Stage A). `CasRefLatePredecessorObserved` (B4) is deleted from the tree (a historical comment in `gtest_cas_ref_writer.cpp` remains); end-to-end LIST-liar fault injection = Stage A T13.
- **[MOUNT-CLAIM-EPOCH-REGRESSION] should `claimMount` permit epoch regression?** — QUESTION (surfaced by Stage A T12, 2026-07-29) — `claimMount` (`CasServerRoot.cpp` ~:395) reclaims a same-uuid body that is gc_fenced / clean-marked / proven-dead WITHOUT comparing epochs, so a fenced twin holding a HIGHER allocated epoch is legally reclaimable while the fresh writer proceeds with a LOWER `writer_epoch` — an epoch regression at the mount claim. Intersects the same-uuid recreation epoch-counter reset (quiesce = primary defence). Decide: must the claim gate require fresh `writer_epoch` above the reclaimed body's epoch (new `MountClaimResult` field), or is regression benign under the seal grammar? Sharp edge to verify: `prev_epoch_seal` is required iff `writer_epoch > life_epoch`, so a regressed writer may skip the seal obligation — confirm that path cannot readmit a Late-Predecessor window. T12 deliberately did NOT add a `chassert` here (it would abort a path the claim logic permits); surviving guards: unclean-reclaim classification, exhaustive `-Wswitch`, operator log line.
- **[refsnaplog Phase 2] measured ref-log/snapshot optimizations** — DESIRABLE (measurements-gated) — inline zero-byte log keys; GC-side fallback compaction for never-mounted tables; indexed/chunked multi-object snapshots; lazy snapshot blocks + byte-bounded row cache; per-round ref index; streamed snapshot construction; adaptive thresholds; decoded-body reuse; chunked namespace removal. Plus a **cross-epoch fault-injection integration test** reproducing the late-predecessor counterexample.
- **[timeout-retry RFC residuals] bounded lease-aware S3 timeout/retry controller** — PARTIAL — `CasRequestController` (single-attempt conditional writes, budget, fence-gating, exact-key resolution) landed for the ref lane. RFC `specs/2026-07-12-cas-s3-timeout-retry-control-rfc.md` residuals still open: (a) AWS SDK region-redirect retry can bypass `ShouldRetry` when a client is `aws-global` (CAS disks are not aws-global today — add a startup guard/probe if that changes); (b) `promoteStaged`'s `copyObjectConditional` (server-side conditional copy) is a separate conditional-write mechanism NOT bounded by the single-attempt work — verify its retry semantics before relying on write-once promote; (c) bounded read/HEAD/LIST retries + startup validation for the non-ref plain-object paths (`casPutObject`/`casRemoveObject` still use the disk's default retry policy).

## 2. GC scalability & byte cost {#gc-scalability}

- **[gc-frontier-one-list] cheap change discovery: ONE LIST instead of per-namespace frontier GETs, + parallel walk** — DESIRABLE (USER-DIRECTED plan 2026-08-03) — {#gc-frontier-one-list} — Today every `Live`/`Removing` namespace costs the round at least one exact `GET cursor+1` (the frontier probe — the only thing that sets `frontier_proven`), even when `tail == cursor` says nothing changed, and the whole ref walk over `walk_targets` is a plain sequential `for` on the round thread (`Gc/CasGc.cpp:2134`; `meta_pool` parallelizes meta ops only) — so a quiet 10k-table pool pays ~10k **serial** GETs (~0.5 ms each) per round for the discovery alone. Plan, two independent levers: **(1) trusted-LIST frontier mode (per-backend, passported)** — on a store whose LIST is certified compliant (lists started after a completed PUT include it), the round's single frozen LIST tail at `tail == cursor` IS the frontier proof: a correct tail majorizes everything completed before the LIST started, and records landing during/after the enumeration are unacked by construction (ack requires the `_ckpt` CAS after the log PUT) and covered by the three temporal arms (`Gc/CasBlobInDegree.cpp:422`), so skipping the probe GET for quiet namespaces is sound — discovery cost drops to ~one paginated LIST per round. NON-NEGOTIABLE scope limits: the MIDDLE stays arithmetic unconditionally on EVERY backend (predecessor-omission below a correct tail is LEGAL S3 behavior under concurrent paginated enumeration — no snapshot contract exists for writes landing mid-walk — this is exactly the observed `0x1430c`/`0x1430d` shape); the probe survives for HELD namespaces (a hold clears only by exact-key resolution of its offending position, spec §5) and for namespaces with no listed logs at all (the hiding-store / quiescent shape); `tail < cursor` keeps feeding the store-quality detector. Certification = `Cas::Probe` LIST-consistency passport (§6 `[LIST consistency on real S3]` is the existing hook — connect it); RustFS today FAILS the passport (proven-by-measurement omission of 19-s-durable keys, `2026-07-26-list-incompleteness-proof/`), so conservative (probe-always) stays the default and the mode is opt-in per backend, same shape as `[ckpt-read-policy]`. SETTLED FACTS + passport-design constraints (what the incident proved/did not prove, which lie classes are legal on compliant S3, which verbs stayed honest, and why a ONE-SHOT probe cannot certify — 3 hammer runs / ~19M keys / zero holes, then a live firing; passport = per-BUCKET operator attestation and/or continuous verification with auto-revoke, fail-close to probe-always): `2026-08-03-list-trust-verdict.md` — READ IT before re-arguing any of this. **(2) parallel exact-key walk (backend-independent, no trust change)** — fan the per-namespace probes and the `cursor+1..tail` record GETs over a bounded pool (reuse/extend `meta_pool` or a sibling; per-namespace work is independent by construction — separate keys, separate cursors; fold application stays on the round thread or merges per-namespace results deterministically): wall time for N quiet namespaces drops N× serial → N/K. Lever (2) alone fixes the wall-clock without touching the trust model, and remains worth it under (1) for the hot namespaces that still fold records. MEASUREMENT: per-phase timing rows exist (`d412f85f749`) — record the discovery phase's GET count and wall before/after each lever; effect estimate on the quiet wide pool: discovery ~10k serial GETs → ~pages(LIST) requests (lever 1) or same GETs at 1/K wall (lever 2). — DESIRABLE — The single dominant remaining byte cost: a HOT pool rewrites the full snapshot run O(edges) per pass. Build O(delta)-write log-structured runs + periodic compaction on the landed T2/T0 primitives (streaming reader, `seek`, `getStream`, ranged `get`, seal-ref resolution). Canonical item (dedups: snapshot-streaming T1, ack-floor T1, 04/07 rows, refactoring-ideas "incremental LSM snapshot", O(buffer) run-file streaming residue). Streaming reads + reference-parent runs (T2/T0) already DONE.
- **[Lever B] Incremental point-updatable in-degree** — DESIRABLE — Makes even a non-idle small-delta round O(delta) (Lever A only short-circuits idle/no-destructive rounds). Also provides the global change-signal that would let GC drop the per-round `LIST(cas/refs/)` O(shards) sweep (gate #16 residue: discovery-placement quadratic is DONE; the O(universe)-per-round fold cost remains). Full-scale data: GC round is O(pool objects) — 87ms@400 parts → 93s@10k tables → 398s@100k parts.
- **[ADAPTIVE-GC-CADENCE] journal-pressure-triggered fold** — DESIRABLE — Fold trigger should key on per-shard journal pressure (size/age), not changed-shard count, so one hot shard bounds deferral. Prod direction: modest `gc_interval_sec` (~30–60s) + journal-pressure trigger; constants pool-specific (soak-sweep the knee). Follow-up to Lever A.
- **[distributed gc_shards>1 parallel GC] shard claim/scheduler** — DESIRABLE — Attempt-scoped generations are the prerequisite (DONE); the multi-worker shard claim + scheduler is not built.
- **[B148] HEAD storm at retire — stored-token optimization** — PARTIAL — The retire/recheck O(universe) HEAD phases are gone (v3 round); the residual condemn HEAD is bounded by newly-condemned candidates. Stored-token skip requires a manifest schema change (deferred). Related: **[PROMOTE-REVALIDATION-MINIMIZATION]** skip per-leaf promote HEADs when the installed round is unchanged since the dep observation.
- **[process_epoch → writer_epoch] stamp unification** — DESIRABLE — The writable path already sets `process_epoch = writer_epoch`; unify the manifest `writer_instance_id` stamps.
- **[PART-REMOVAL-REPOINT] part removal pays a wasted repoint of the doomed ref** — DESIRABLE (measured 2026-07-15 milestone soak) — 17 707 `ref_repoint` events, ALL on `delete_tmp_*` refs (writer node, mutations+TTL profile, 20 min): the rename-to-`delete_tmp` + per-file-unlink removal flow commits removal marks (a full stage+precommit+promote repoint, ≈3 PUTs) on a ref that the very next step removes entirely — ≈53K PUTs ≈ 22% of the writer's PUT class, pure overhead. The same-transaction `removeDirectory` supersede-clear (T8) already elides this when unlinks+rmdir share a txn; the cross-transaction removal flow misses it. Fix direction: defer/elide the marks-commit when directory removal follows, or widen the supersede window. (Contrast: scenario cards S03/S04/S05 assert `CasRefRepoint==0` and hold — this class only appears under part-removal churn.)
- **[GC-EMPTY-SHARD-PROBES] constant per-round 404 probe floor** — DESIRABLE (measured 2026-07-15) — ≈1 174 `DiskS3ReadRequestsErrors`/round, CONSTANT regardless of round work (work-driven HEAD/GETs all hit; the misses are the structural probe set of per-shard journal/run/seal keys that are absent for empty shards; grows ≈+4/round as the writer touches new shards). On a small/idle pool this is the dominant GC request class (~3.5K req/min at 3 rounds/min). Removed by [Lever B]'s change-signal (stop probing unchanged/empty shards); until then it belongs in the `07-s3-budget` request-count model (404s bill as requests).
- **[REF-QUEUE-WAIT-MEASURE] insert-path ref-lane queue wait ≈48 ms/insert** — DESIRABLE (measurement, 2026-07-15) — `CasRefQueueWaitMicroseconds` attributed to Insert queries = 339.6 s over 7 131 inserts (~48 ms avg) in the milestone soak; a data point for the refsnaplog Phase-2 flush-cadence/adaptive-threshold work — verify the batch-flush scheduling isn't leaving easy latency on the table before touching code.

## 3. GC correctness / observability follow-ups {#gc-followups}

- **[GC round progress observability] round-duration watchdog + fold-window events** — HARD — A long/wedged round is only visible after the fact; emit a round-duration watchdog, LIST/fold-window progress events, and an alert on an unbalanced `gc_fold_begin`/`gc_fold_end` pair.
- **[ack-floor soak validation] 3 scenario cards** — TEST — SIGSTOP-a-writer holds-then-releases the floor; hard-KILL-writer → fence-out → fsck no-dangle; O(delta)+O(servers) request-count regression guard. Note: floor semantics changed under freshness-v3 (round-paced, per-hash `.meta`) — update the cards. Implemented + unit/TLA-covered; not soak-validated.
- **[F3] `ca-gc-dryrun` reachability under-counts vs real GC/fsck** — HARD — Systematic across S18/S25/S26/S33; real GC always safe (dangling=0). Fix: dryrun uses the SAME reachability walk as GC/fsck; assert `dryrun ⊆ (unreachable ∪ pending-gc)`.
- **[D3 / S31] full GC round + `ca-gc-dryrun` completeness under `gc_shards>1`** — TEST — RESOLVED 2026-07-18: `previewDeletes` covers ALL shards (preview == same-instant fsck `pending_gc` across both shards, S31 run `20260718T001753`); the old "previews only shard 0" narrative was a card-oracle misdiagnosis (single-round preview compared against cumulative multi-round reclaim). Card oracle rebased onto the same-instant fsck snapshot (see `2026-07-18-s31-dryrun-shards-rca.md`). Sharded fold coverage itself remains exercised by S31.
- **[clamp liveness] scoped suppression under long persistent clamps** — DESIRABLE→HARD (2026-07-18: concrete reproducer) — Fail-closed clamp+suppression is correct and self-heals (false-404 attribution), but suppression-vs-liveness under long clamps is unaddressed (scoped suppression later). Clamp observability (clamped key/shard event) is DONE. The S38 sub-finding of 2026-07-18 — a poison late log clamps its own key and thereby starves `reportLateLogsIfAny` indefinitely (40 healthy Success rounds, sweep pass suppressed in every one, `RefLateLogDetected` never fires, `2026-07-18-s38-late-log-clamp-starvation.md`) — is MOOT as of Stage A task 6 (`d74c726ef9e`): the LIST-based late-ref-log detector it starved no longer exists, and a late log is now fenced by an in-band `EpochSeal` rather than reported after the fact (S38 asserts that fence directly). The general item stands on its own reproducers; the starvation shape is kept here only as the record of why a report-after-the-fact detector was the wrong shape.
- **[gc-rebuild follow-ups]** — MINOR — Dedicated gc-round-log row for `rebuildBaseline` (currently only `LOG_INFO` + a `gc_rebuild` event); the "unowned-alive manifest edge over-protect" documented leak (bounded, fsck-visible, cleared by a future rebuild); soak validation (`mc rm gc/state` mid-soak → guard fires `CORRUPTED_DATA` → `SYSTEM … GC REBUILD` recovers to dangling=0).
- **[fsck oracle gaps]** — MINOR — fsck under-reports orphan manifest bodies for ref-less namespaces (enumerate `cas/manifests/` too, not just `cas/refs/`+`roots/`); fsck Orphan-class test gap.
- **[REBUILD R4 residual — manifest-less blobs unreclaimable]** — TRACKED, by design until R4 — Since Task 11 a rebuild condemns nothing (spec §7 — the zero-edge LIST/HEAD sweep was the r5-finding-4 data-loss vector), so a blob whose manifest no longer exists anywhere in the pool has no row in the rebuilt baseline and the incremental pipeline can never reach it. Such blobs are RETAINED and show as fsck `unaccounted` that does not drain after a disaster rebuild. This is the NAMED staging-contract residual of register R4 (the build/upload registry, which is what can enumerate in-flight uploads safely). NOT a bug and explicitly NOT to be closed with a substitute reclamation: any rule that reclaims from an enumeration reintroduces the same vector. Closes when R4 lands.
- **[repointRef non-resolving-key audit gap]** — MINOR — `CachedPartFolderAccess::repointRef` (`CachedPartFolderAccess.cpp:283`) increments `CasRefRepoint` and logs "Repointed committed ref…" unconditionally after its `if (resolved)` byte-equal check, even when `resolve(key, ForceFresh)` returns `nullopt` — i.e. it would count/log a repoint for a key with no existing committed ref. Unreachable today (every caller — Task 4's standalone writes, Task 8's removal-mark resolution — only calls `repointRef` on an already-resolving key); a defensive `throw LOGICAL_ERROR` on `!resolved` would make the precondition explicit and the counter/log trustworthy rather than merely-currently-true. (Found during all-tree Tasks 7/8 integration review.)
- **[ProvenanceOp operability gap]** — MINOR — Both the Task 4 committed-ref standalone write and the Task 8 removal-mark repoint call `repointRef(..., Cas::ProvenanceOp::Other)` — no distinct op kind for a removal-repoint vs a write-repoint in `system.content_addressed_log`. Spec doesn't require one; would help an operator distinguish "this repoint dropped files" from "this repoint added/changed files" in the audit trail without decoding the entry diff. Product-owner call, not decided during Task 8 integration. (Found during all-tree Task 8 integration review.)
- **[codex-11] namespace drop misses an unregistered build → ownerless Live namespace** — LOW — `Pool::beginPartWrite`'s allocate/register window (`CasPool.cpp:772-777`) is real: a build can allocate before the drop sweep (which only snapshots `inflight_builds`) runs, then legitimately pass the birth-time marker gate afterwards, reviving a Live-but-ownerless EMPTY ref-table — a small, non-self-healing metadata leak (GC never sweeps Live namespaces). Confirmed narrow, LOW severity (2026-07-17 codex-review triage, finding №11); the reviewer's atomic-registration fix would NOT close it (the same TOCTOU recurs between the `cancelled` check and the append for already-registered builds). Fix direction: a GC backstop that reclaims empty ownerless Live namespaces, or a namespace generation folded into the birth-time marker gate.
- **[RECOVERED-INDEGREE-ATTRIBUTION] move the "delete_pending recovered in-degree" invariant check to the writer** — DESIRABLE (2026-07-24, PR#2073 CI triage) — The GC-side `LOG_WARNING "…recovered in-degree — structurally impossible under the ack floor; investigate"` (`CasBlobInDegree.cpp:418`, `CasGc.cpp:485`) fires as a *false alarm* (56×/run on tiny system-log blobs in the CAS-s3 stateless job): root-caused to a **dedup-adopt-vs-condemn TOCTOU** — a write-once PUT hits `PreconditionFailed`, `observeAndAdmit` (`CasPartWriteTxn.cpp:354-421`) meta point-reads the blob as not-Condemned (read preceded GC's `Condemned` write) → adopts the token → the fresh edge recovers in-degree after GC's fold cut → GC **spares** (verified no data loss: zero read-side blob 404s). By construction a `delete_pending` blob carries no surviving prior edges (`CasBlobInDegree.cpp:518-521`), so every such recovery is a fresh this-generation edge — GC cannot locally tell a legitimate race from the ONE genuine bug this masks (**adopt-without-resurrect**). Fix direction: (a) downgrade the GC log to a dedicated `ProfileEvent` (`CasGcRetiredSparedByReref`) + `LOG_DEBUG` (metric already exists: `CasGcRetiredSpared`, `CasGc.cpp:503`) so the false alarm and the misleading "structurally impossible/investigate" wording go away; (b) put the real detector where it can decide — at the writer's edge-commit, re-check the meta (or record the meta-generation the adopt was based on) and emit a typed `BlobAdoptRacedCondemn` event (with `source_id`) iff the blob became Condemned between observe and commit; then the GC spare is a silent safety-net. Also enrich any remaining signal with the recovering `source_id`+round (see [[reference_cas_ci_observability_gaps]] #4). RCA in [[project_pr2073_ci_triage_2026_07_23]].
- **[CONDEMN-GRACE-WINDOW] cool-down before condemning a just-zeroed blob (kill hot-dedup churn at the source)** — DESIRABLE (2026-07-24, PR#2073 CI triage) — The driver of the RECOVERED-INDEGREE noise (and its wasted condemn→HEAD→spare→re-condemn cycles) is tiny system-log blobs (`system.content_addressed_log`/`trace_log`/`zookeeper_log`, 286–415 B) whose in-degree hits 0 and is re-referenced by dedup almost immediately. A short grace/cool-down before graduating a blob whose in-degree just transitioned to 0 (defer condemn by one round or a few seconds) lets the re-reference land as a surviving edge, so the blob is never condemned — removing the spare churn AND the associated HEAD-storm/S3 request cost on hot blobs (ties to the `07-s3-budget` request-count model and [[project_cas_insert_slowness_writepath]]). Higher risk: this changes condemn timing → touches GC invariants (retention vs reclaim latency, ack-floor interaction), needs TLA-level reasoning and is subject to the protocol-step change veto ([[feedback_head_before_put_protocol_untouchable]]) — do only as a deliberate design with a model, not a "cheap" tweak. Measure first: count re-condemn/HEAD/spare cycles on the 28 hot hashes from run 30019911967 to size the benefit. RCA in [[project_pr2073_ci_triage_2026_07_23]].
- **[REBUILD-SEAL-POINT-READ] attempt-free per-generation marker: the point-read closure for REBUILD seal discovery** — HARD (2026-07-28, Stage A Task 8 fix rounds 1b/2) — `rebuildBaseline` must find the pool's newest fold seal WITHOUT `gc/state`, because that is exactly the disaster it recovers from, and the seal is where durable holds live. It cannot point-read one: `foldSealKey(generation, attempt)` needs an attempt component that is `lease.seq` (`CasGc.cpp`, `const uint64_t attempt = state.lease.seq`) — a global lease counter advancing on EVERY round including deferred ones, so consecutive generations carry attempts separated by unbounded gaps and there is no `attempt + 1` to probe. Generations ARE dense in minting (`snap_generation + 1` for a fold, `max_gen + 1` for a rebuild), so today's discovery is arithmetic in the generation half and an enumeration-within-one-directory in the attempt half: `probeGenerationForSeal` lists a single `gc/gen/<G>/` prefix. As LANDED, discovery takes the wide listing's maximum `G`, probes a BOUNDED two generations above it (`G+1`, `G+2`) and REFUSES terminally on a seal found there — the listing caught lying, not merely incomplete — and otherwise steps DOWN through the generations that same listing reported until one carries a seal (the ordinary crash shape: a round writes its runs at reduce and its seal only at phase 10/18, so the newest generation routinely exists without one). That is probe-A-style DETECTION, not proof, and it is labelled as such at the site. **CAVEAT, for the record:** the step-down's premise — below-max is "merely incomplete" rather than lying — rests on the NARROW single-generation listing being truthful, so residual (1) below is now reachable on the ORDINARY path, not just the adversarial one. Signed off as the right trade: refusing instead was accidentally stricter than the documented trust model and paid for that strictness with the commonest crash there is. **Two residuals follow, both from the same root.** (1) A store that lies about ONE generation's own prefix can still hide that generation. (2) The virgin verdict — "this pool never sealed a baseline, carry no holds" — rests on a wide LIST being empty plus a narrow `gc/gen/1/` probe being empty plus no `gc/state`, and the generation-1 probe NARROWS rather than closes it: on a PRUNED pool generation 1 legitimately does not exist (`pruneSupersededGenerations` deletes whole old generation prefixes once they age past `gc_snap_generations_to_keep`), so a total enumeration blackout on a lived-in pruned pool still reads virgin and silently drops every hold. It is logged at WARNING with its evidence and implication, counted by `CasGcRebuildVirginByEnumeration`, and reported as `virgin_by_enumeration` on the `SYSTEM CONTENT ADDRESSED GC REBUILD` row — visible, but not prevented. **FIX:** a DERIVABLE per-generation alias that can be point-read — e.g. a write-once `gc/gen/<G>/sealed` minted at adoption naming the adopted attempt. Newest-ness then becomes a dense-`G` exact-`GET` walk identical in shape to the ref walk (probe `G + 1`, absence decides the top, no listing load-bearing anywhere on the path), residual (1) dies outright. **Residual (2) does NOT die with it, and the earlier claim that it did was wrong:** a point read confirms a generation you already know to ASK about — it does not supply the FLOOR. A `gc/gen/<G>/sealed` marker sits INSIDE the very prefix `pruneSupersededGenerations` deletes wholesale, so on a pruned pool an upward walk from generation 1 finds nothing at all, exactly as today. Killing residual (2) needs something that SURVIVES pruning: either the marker in an UNPRUNED location, or a pool-level FLOOR POINTER naming the oldest surviving generation. Both shapes are named here; the choice is deferred to the spec-amendment decision. Note this is a small **protocol/format amendment** (a new write-once object minted on the adoption path), which the controller has separately ledgered as a **SPEC-AMENDMENT CANDIDATE for the lanes-convergence decision point** — this entry is the engineering half of the same item. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-8-report.md`, sections "Fix round 1b" and "Fix round 2" (the latter's "Residual and the structural fix").
- **[STAGE-B-7B-SEQUENCING] `UniversePolicy::kDefault` must NOT flip before Stage B's incarnation-keyed cursors land** — {#stage-b-7b-sequencing} — HARD CONSTRAINT, NOT A TASK (2026-07-29, Stage A Task 9) — Stage B's Task 7b is a one-line change: `kDefault = StageA_Suppressed` → the value that consults the per-namespace frontier proofs (`Gc/CasGc.h`, marked `STAGE B'S TASK 7b EDITS EXACTLY THIS LINE`). It must not happen first. **Why.** Task 9's destructive gate owes a frontier proof for every namespace that can hold a live edge, so the round's universe became `(namespaces with a shard-0 cursor in the adopted fold seal) ∪ (this round's LIST hint)` — a UNION, deliberately, so that a hint going quiet about a namespace can no longer SHRINK the obligation. The cost of that union is that a coverage row now rides FOREVER for a namespace with no ref objects at all. Before Task 9 such a namespace left no row, and a later recreation folded from `{0,0}`; now its cursor persists, which widens the remove-then-recreate-within-one-writer-epoch residual already documented at the `expected` initialization in `CasGc.cpp` from ONE ROUND WIDE to PERMANENT. The damage that residual describes: under `nextRefTxnId` (INV-1) ids are derived per namespace from that table's own state, so a namespace removed and recreated inside one writer epoch — after its logs were cleaned and its runtime re-recovered from nothing — restarts at `{E, 1}`, at or BELOW the retained cursor. The walk starts at `cursor + 1`, so either it never folds the recreated edges at all (records below the cursor are never re-read) or it folds from `{E, k+1}` and misses `{E, 1}..{E, k}` — both leave the recreated refs' manifests looking unreferenced, i.e. live blobs eligible for deletion. **The practical delta is small and is why this is a sequencing note rather than a bug:** a namespace that goes away through the removal protocol leaves its `_cleanup` marker behind permanently (nothing deletes markers — they are the recreation precondition), so it stays in the hint and already had its cursor re-sealed every round. Only a namespace whose ref prefix is COMPLETELY empty — corruption, a lying store, or a drop that bypassed the protocol — is newly retained. **Containment today is the gate itself**: Stage A destroys nothing, so nothing acts on the stale cursor either way. That containment evaporates the instant 7b flips. **The structural closure is the one the code already names**: cursors keyed by `(namespace, incarnation)` rather than by name, which makes a recreated namespace a DIFFERENT key instead of the same key with re-derived ids. Do not close it by comparing ids at the walk. Related: the same union is why coverage rows accumulate without bound (one per namespace ever created) — pre-existing, since the `_cleanup` marker already pinned them, but now unconditional; Stage B's catalog is where that gets bounded. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-9-report.md` §12.
- **[SUPPRESSED-HANDOFF-CONSUMPTION] a suppressed round CONSUMES the hand-off reclaim instead of deferring it** — {#suppressed-handoff-consumption} — MINOR (bounded leak, fsck-visible) — (2026-07-29, Stage A Task 9) — The post-CAS hand-off (`runRegularRound`, PHASE 14/18 `handoff_reclaim`) reclaims a generation the wholesale prune SKIPPED while a live ref pinned it and which this round's ref finally moved off. Task 9 gated it with every other destructive site, and unlike the others that gate DROPS the work rather than deferring it. **Why it cannot be retried.** The hand-off is a one-shot DIFFERENCE between the PARENT seal's `blob_target_runs` and the new seal's. A suppressed round still FOLDS — only the irreversible half stops — so the ref moves off the old generation on that very round, and the next round's parent seal no longer mentions it. Nothing revisits it: `snap_pruned_through` is already past that generation and the wholesale prune never walks backwards (`pruneSupersededGenerations` walks forward from the cursor). The prefix — fold seal, per-attempt retired/outcomes sets, every shard's runs for that generation — is left to `fsck`. **This is not new in KIND:** the site's own doc comment already records exactly this outcome for a crash between the round CAS and the hand-off ("the cursor already advanced, so a plain retry will NOT re-attempt it; fsck is the backstop"). What Task 9 changes is FREQUENCY: in Stage A every round is suppressed, so every such transition leaks rather than one in a crash. Bounded (one small run per shard per occurrence, on a pool with idle shards whose runs aged past `gc_snap_generations_to_keep`), no correctness dimension, and it stops being systematic the moment Stage B's Task 7b flips `kDefault` (see `{#stage-b-7b-sequencing}`). Asserted rather than left to be discovered: `CasGcFrontierGate.TheHandOffReclaimIsInertUnderSuppression` pins both halves — the suppressed round hands nothing off, AND the following authoritative round can no longer find the work. **If a fix is ever wanted** it must not be a LIST of `gc/gen/` to re-derive the difference (that is the GC-DISCOVERY-LIST-QUADRATIC cost the design removed on purpose); it would need the unreferenced-generation fact recorded durably at the transition, which is a protocol addition and not obviously worth it against `fsck`. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-9-report.md` §12.
- **[RECOVER-REF-TABLE-LIST-RESIDUAL] the free-function recovery equation is still LIST-driven — a named 7b precondition** — {#recover-ref-table-list-residual} — 7B PRECONDITION (2026-07-29, Stage A Task 13, confirmed empirically) — `recoverRefTableDetailed`/`recoverRefTable` (`Pool/CasRefProtocol.h` free functions) still recover by full LIST + replay, while the production mount path (ledger CAS-walk, Stage A T6/T8) and fsck's `checkRefStream` walk arithmetically. Two consumers inherit the lie: (1) fsck's per-namespace replay (`Tools/CasFsck.cpp` ~:572) — a record hidden in the MIDDLE throws `CORRUPTED_DATA` ("does not continue the ref-log stream") and the per-namespace catch yields the `Unchecked` verdict (fails closed but BLIND: `clean()` still true, `reachable=0` for a healthy namespace); a record hidden at the TAIL contradicts nothing, so the replay ends one txn early and fsck reports a clean bill over a ref set missing an ACKED publish. (2) the orphan-manifest sweep's DELETION premise (`Gc/CasOrphanManifestSweep.cpp` ~:153, `activeManifestKeys` = snapshot + replayed tail) — a silently short owner set is the premise for deleting a manifest body out from under a live committed ref. The arithmetic pass is unaffected in both shapes (`chain_broken=0`, `ref_records_walked` matches the honest twin) — Task 7's work does its job; the residual is confined to the replay. CONTAINMENT TODAY: Stage A suppresses every destructive site, and further gates (watermark floor, epoch premise) sit in front of the sweep delete — exploitability through those gates deliberately NOT analysed yet. Pinned by Task 13 tests whose comments state the correct end state and instruct the fixer to replace the assertions. SCOPE SHARPENED (T13 close-out): (i) full consumer table now measured — additionally `Gc/CasGc.cpp` ~:3746 (`SYSTEM CONTENT ADDRESSED GC REBUILD` rebuilds owner set + cursor through the same replay) and `Tools/CasFsck.cpp` ~:126/~:166 (dangle revalidation — suppress-only: loses detections, authorizes nothing); (ii) REBUILD is only PARTLY mitigated by condemn-nothing (r5-finding-4 precedent, `gtest_cas_rebuild_condemn_nothing.cpp` header): it reclaims nothing, but it rebuilds CURSORS AND EDGES via the short replay, so its baseline can be missing a `+1` that a LATER incremental round acts on — a DEFERRED form of the hazard, not covered by "rebuild reclaims nothing"; (iii) documentation defect to fix with the code: `Tools/CasFsck.cpp` ~:124 comment "The recovery equation sees every log." is exactly the claim the measurement falsifies (a LIST-driven replay sees every LISTED log). CLOSURE: drive the replay arithmetically (reuse the CAS-walk / `checkRefStream` machinery) or make ALL consumers consume the arithmetic walk (incl. the rebuild baseline), and correct the `:124` comment; resolve — or prove non-exploitable through the named gates — BEFORE `{#stage-b-7b-sequencing}` flips `kDefault`.

- **[RECOVER-REF-TABLE-LIST-RESIDUAL] CLOSED — Task 5b** — {#recover-ref-table-list-residual-closed} — The dated entry immediately above is retained as historical evidence only. The model is committed as `c863cdd7fa60`; production baseline `357cf7b963f4` is completed by closing chain `3747975bbbf`, `8183a1af1800`, `e48b476d90f`, `4ab9b452e660`, `60cbec2bd274`, `7ac127b650a`, and `613faf8166e`. `chooseRecoveryGrounding` accepts only the exact catalog row and `_ckpt`; both writer and read-only recovery perform zero stream `LIST` requests, use only `_ckpt.checkpoint_snapshot_id` as a base, and replay exact keys through `committed_through`. Full/empty/partial/reordered backend listings and a forged well-formed uncommitted snapshot are pinned to yield the same recovery state without observing the forged object. The two list-liar fsck capstones prove that hidden middle and tail records reconstruct the honest reachability result. `LIST` remains only GC/janitor garbage nomination, never recovery scheduling or diagnostics. The Task 7b precondition is discharged; no Task 5b debt remains open.

- **[PROBE-A-CADENCE-UNIT] the sampled store-quality detector never fires when rounds are slow** — {#probe-a-cadence-unit} — T12 FOLLOW-UP (2026-07-29 T14 soak) — `gc_probe_a_period` samples on `round % period == 0` (`Pool/CasPool.h:110`, default 16), but the sampling UNIT is rounds while round duration is unbounded: the T14 soak's GC leader completed ZERO folding rounds in 42 minutes on a ~30 GiB hot pool (one-pass fold O(pool), see `{#gc-scalability}`), so `CasGcProbeADue/Performed/Skipped` all stayed 0 and `CasGcRefScanDisagreements` could never read nonzero. A 1-in-16-rounds cadence over tens-of-minutes rounds = a detector that effectively never samples exactly where store quality matters most (busy pools). Candidate redesign: time-based due rule (sample when `now - last_probe > T`) or an intra-round probe at the enumeration site; keep the aborts-nothing contract. Stated plainly in the Stage A RESULTS per the T14 measurement. **CLOSED 2026-07-29 by the Task 15 re-validation**: with rounds bounded again the detector came due FOUR times on the cadence, performed every time, and reported zero disagreements (`CasGcProbeADue=4`, `Performed=4`, `Skipped=0`, `CasGcRefScanDisagreements=0`; `build/t14_revalidation/criteria_evidence.txt`). The sampling UNIT was never the problem — liveness was, and Task 14 measured the cadence against rounds that never finished. `gc_probe_a_period` stays 16. Kept as the fallback: if a slow-round world ever returns, a time-based due rule is the redesign, because a cadence expressed in rounds cannot bound the interval between samples when round duration is unbounded. **MOOT (Task T5): probe A is deleted outright — `gc_probe_a_period`, the detector and this whole cadence question no longer exist.**

- **[FSCK-SCALE-TIMEOUT] product ca-fsck cannot complete a ~29 GiB pool within its own 600 s deadline** — {#fsck-scale-timeout} — MEASURED (2026-07-29 T14: `build/t14_fsck_cost.log`, pool `du -sb` = 31,147,968,714 bytes, `FSCK_SECONDS=731.1`, `FSCK_EXIT=159` TIMEOUT_EXCEEDED, no `reachable=` line parsed) — the T11 O(backlog) cost model measured to its breaking point: at this scale the audit returns NOTHING, so "fsck clean" is unmeasurable by the product's own tool exactly where an operator would want it most. Direction: bounded/streamed partial verdicts (per-namespace pagination with a resumable cursor), deadline-aware partial reporting (the `partial`/`partial_reason` fields exist — make the CLI surface them instead of exiting 159 empty), and the backlog-drain dependency (a bounded-round GC that actually completes rounds keeps the backlog — and hence fsck cost — small; Task 15). THIRD MEASUREMENT (T14 soak2): raising the harness budget 180->600 s bought NOTHING — entry-gate fsck timed out at 600 s on a 23,503,409,316-byte pool at minute 47 (the cost is the pool, not the budget), and `--max-pool-gb` can only PACE inserts under Stage A (throttle pinned at 1.0 s/insert while the pool climbed) — a budget that works by withholding inserts cannot bound a pool that never reclaims. STRUCTURAL CONSEQUENCE (controller-ruled, user-visible): "fsck clean at end" is not achievable by a 90-minute phase-3 soak under Stage A at any pool-growing workload; the stage's fsck-clean evidence comes from complete audits at auditable scale (05020 + scenario end-checkpoints), the soak's fsck gates are reported UNARMED with this reason, and Stage B's 7b flip relieves the contradiction by itself (reclamation resumes, the audit fits again). Related: `{#soak-fsck-checkpoint-budget}` (harness-side budget), `{#gc-scalability}`.

- **[CA-LOG-TABLES-RESTART-COST] the CA instrumentation's Outdated churn failed the restart health gate** — {#ca-log-tables-restart-cost} — NEW (gc-audit 2026-07-29) — the 6/40 soak's post-kill restart took 178.9 s against a 180 s gate: 40.0 s CAS mount-lease token-stability wait + 138.1 s reloading SYSTEM-LOG tables' Outdated parts (`system.content_addressed_log` alone: 299 Outdated parts; the USER table loaded 1 part in 15 ms). The observability that makes a soak a specimen is what failed its checkpoint. Directions: TTL/partitioning for the CA log tables, bounded event-log part churn, lazy system-log load. Related: the merge-churn datum (generations 74k+/35 min).
- **[CA-GTEST-TMP-SCRATCH-LEAK] every full CA gate leaves thousands of scratch dirs in /tmp; tmpfs INODE exhaustion broke a build mid-flight** — {#ca-gtest-tmp-scratch-leak} — found 2026-07-29 late evening when `llvm-ar` failed with `No space left on device` at 269 G free: the tmpfs was at 1,048,286/1,048,576 INODES. Consumers: ~40k `cas_unit_*` pool dirs + ~10k siblings (`cas_sentinel_probe_unit_*`, `gtest_plc_*`, `ca_commit_rollback_scratch_*`, …) accumulated since 2026-07-21 — CA gtest fixtures create scratch pools in /tmp and never clean them on teardown. Immediate mitigation applied: `cas_unit_*` removed while no test ran (inodes 100%→14%); sibling prefixes REMAIN and the leak recurs every gate run. FIX (small, test-infra): teardown-time removal in the shared CA test fixture (or scratch under the build dir instead of /tmp) — schedule at the next free implementer slot, NOT while lane-g gates run (sweeping live scratch breaks running tests). Same debris family as the lane-local `k/ p/ pool/` cwd droppings — and those are now REPRODUCED rather than assumed (Task-1b fix round, 2026-07-30): the implementer deleted them, ran the CA gate, and found them RECREATED with fresh timestamps holding empty trees (`pool/blobs/ab/`), so some test in the gate writes CWD-relative on EVERY run in ANY lane and deleting them is futile without fixing the source. The obvious suspects are EXONERATED so nobody re-checks them: `cas_test_helpers.h:134`'s `makeLocalObjectStorageForTest` correctly roots under `temp_directory_path()`, and the `"pool"` arguments at `gtest_cas_forget.cpp:140` / `gtest_cas_operation_gate.cpp:54` plus `Layout layout("pool")` in `gtest_cas_mount.cpp` are KEY prefixes, not filesystem paths. Cosmetic and pre-existing; fix it with the teardown work above, and bisect by running suite groups with a clean CWD rather than by reading fixtures.
- **[ORPHANED-ADJUDICATION-COMMENT] `CasRefLedger.cpp:108-120` documents an adjudication its neighbouring code does not perform** — {#orphaned-adjudication-comment} — found 2026-07-30 by the Task-1b implementer while attempting (and correctly reverting) an exposure of `chainLinkFor`: the comment describes a `mine | successor's seal | foreign` adjudication with a narrow `catch`, which is NOT what the function beside it does. SMALL but real — a comment that misdescribes its neighbour is worse than no comment, and this region is exactly where the next reader will look when INV-2's chain-link grammar is next touched. Take it with Task 1c's sweep or the restatement pass, whichever reaches the file first; re-derive what the comment SHOULD say from the code rather than deleting it blind. Related: `chainLinkFor` stays in an anonymous namespace, so INV-2's grammar cannot be swept in isolation — asserted through `prepareRefChunk`'s validator instead (accepted disposition, Task 1b).
- **[LANE-STATE-MACHINE-RESTATEMENT] design pass: restate the ref-lane state machine's invariants first-class and collapse its encodings — USER-APPROVED 2026-07-30, planned AFTER the relink TLA gate** — {#lane-state-machine-restatement} — the diagnosis (from the TLA-gate plan rounds): a competent modeler failed twice to describe the existing wedge/Poisoned/ApplyPending/fence/floor-reconcile machine (`CasRefLedger::resolveWedgeOnce`'s two-step stale resolution :1936-1950/:1758-1766 was the second miss) — when a formal model resists writing, the complexity is in the modeled thing. Core observation: THREE mechanisms encode variants of ONE uncertainty ("durable may be ahead of the cached view") — Poisoned, unresolved wedge, ApplyPending — differing only in who owes the fix and what is known. Seeds: the user's own Stage-B out-of-scope follow-up (production keeps one poisoned/needs_recovery bit; ApplyPending becomes debug-only) is the CORE of this pass, not a footnote. METHOD (order is the point): the TLA-gate model (CaRelinkReofferCore v3) is the first formal spec of this machine — simplify IN THE MODEL first (invariants first-class: which fact combinations are legal, who owns each resolution), show the sabotage battery still holds, THEN touch code. Sequencing: after the gate verdict, BEFORE seam/relink implementation. Risk named: the wedge protocol is the most soak-hardened code in the tree — model-first is why this is affordable. **ESCALATED TO ACTIVE 2026-07-30, by a pre-committed stopping rule: FOUR codex rounds each found the gate model unable to express some arm of `resolveWedgeOnce`, and round 4 found the structural reason — the enumeration's cardinalities are NESTED DECISIONS, not mutually exclusive alternatives, so no product of them bounds the terminal paths.** The conclusive finding is not "one more arm was missed" but "the machine COMPOSES decisions rather than selecting among them, and a faithful description keeps being out of reach". Gate planning is STOPPED (`docs/superpowers/plans/2026-07-29-cas-relink-seam-tla-gate.md {#planning-stopped}` carries the stop note, the four missing paths, and two real v4 defects preserved as resumption input). THIS PASS IS NOW THE PATH FORWARD, and it inherits the best possible input: four rounds of adversarial evidence about which distinctions the machine actually makes, plus one CODE observation that belongs to the pass rather than to the model — a durability-producing retry whose outcome is inert (`Created` then an inert recheck ⇒ `StillWedged`) records the durable fact ONLY in the wedge-plus-marker, never in the resolution's return value, which is a compositional-state smell of exactly the kind this pass exists to remove. SEQUENCING QUESTION FOR THE USER (their approval said "plan the pass after the gate", and the gate is now unreachable without a faithful model): restatement FIRST is the recommendation. **METHOD NOTE FOR THE PASS, from independent corroboration inside Stage B Task 1b:** the same reasoning defect reproduced in a C++ test sweep — `{genesis, non-genesis} × {seq 1, seq >1} × {seal present, absent}` called exhaustive, when INV-1 contiguity made most cells unreachable — so it is a property of the REASONING, not of TLA, and it is now a standing memory rather than only a note in the stopped plan. What fixed it at that scale is the move this pass should make at its own: stop treating the product as the space — either ENUMERATE THE REACHABLE PATHS with each cited to the code site that produces it, or CONSTRUCT a valid base state per row, which both makes the row real and turns it two-sided (an ill-formed combination must be REFUSED, not merely absent). The cheap detector to apply before any completeness claim: take the cells the product predicts and ask whether each is reachable; equivalently, ask which dimension is computed FROM another.
- **[DEAD-INSTALL-PROBE-AND-STALE-REGION-COUNT] master's post-durable install seam has no test, and one comment still counts three regions** — {#dead-install-probe-and-stale-region-count} — found 2026-07-30 while reading the restated tree: after `bb4dd513118` **no test anywhere sets `install_region_probe_for_test`** (`grep` over `src/Disks/tests`), so the seam is dead in master — a test hook with no test is a hook that will rot silently, and lane-g's fence test is what would revive it. Separately `CasRefLedger.cpp:1903` still says "Post-durable install region **2 of 3**" although the restatement deleted the third, leaving only `:1910` and `:2929`. **HALF RESOLVED, HALF PLACED (2026-07-30).** The dead-hook half is CLOSED: `gtest_cas_ref_ckpt.cpp`'s carve-time fence now sets the probe, and it discloses in its own comment that the probe is SHARED by both remaining regions — better than this entry asked for. The stale count is still live, and now the tree contradicts itself: that fence says "BOTH" while `CasRefLedger.cpp` still says "2 of 3". **Placement lesson recorded rather than repeated:** this entry deferred to "whichever task next touches the install regions (the Task-1b redo does)" — the redo closed without taking either half, because a pointer in a ledger is not a step in a task. Both are now Steps 0a/0b of plan Task 4, which opens that file.
- **[LANE-TERMINAL-REPORTED-AS-RETRYABLE] one arm sets `Faulted` and hands survivors the RETRY-LATER class** — {#lane-terminal-reported-as-retryable} — found 2026-07-30 while verifying the restatement's own retryability split. `commitRefChunk`'s "lane not `Ready` at new-id allocation" arm sets `RefLaneState::Faulted` (`CasRefLedger.cpp:2593`) but completes the survivors with `makeCasWriteRetryLaterExceptionPtr` (`:2598`) — i.e. a TERMINAL state reported with the class upstream treats as retryable (`NETWORK_ERROR`, `checkDataPart.cpp:110`). Self-limiting rather than a loop: the next flush's `resolveWedgeOnce` takes the `invalid_lane_state` arm and returns `INVALID_STATE` (`:1721`), so the cost is ONE spurious retry on a path that is `chassert`-guarded as bug-or-injection-only. **PLACED 2026-07-30 as plan Task 4 Step 0a**, and re-verified live at placement time. But it contradicts the contract's stated split (terminal `Closed` → `INVALID_STATE` at `:1976`/`:2850`, `Faulted` → `CORRUPTED_DATA` at `:1988`ff/`:2828`/`:2882`), and a contract worth stating is worth not contradicting in one arm. ONE-LINE FIX.
- **[LANE-WITNESS-NAMES-MORE-THAN-IT-PROVES] the new lane battery has a witness that proves less than its name, and a missing adoption witness** — {#lane-witness-names-more-than-it-proves} — found 2026-07-30, and it is a WEAKER ECHO of the defect that stopped the old gate, so it is filed rather than shrugged at: `saw_retry_created` is set in `ObserveDurable` (`CaRefLaneCore.tla:205`), so it witnesses that the RETRY CREATED DURABILITY — not that the adoption install happened — while `CaRefLaneCore_RESULTS.md` calls it "retry-created adoption", which overstates it. And NO witness asserts the `Wedged → Ready` durable-adoption arm at all. Two small fixes: correct the RESULTS wording (prose, per {#prose-is-the-unchecked-surface} discipline — a name that claims more than the assertion supports is the IMPRECISE class), and add a witness that fires on the adoption install itself. **PLACED 2026-07-30 as plan Task 10e.** Does not affect the blocker-dissolved verdict, whose proof runs through `ReadyCaughtUp`. Also noted, judged immaterial: no test drives `Closed`/`Faulted` through `confirmExactRef` specifically — harmless because the refusal does not branch on which non-`Ready` state it is (`CasRefLedger.cpp:443`).
- **[LANE-STATE-MACHINE-RESTATEMENT-STATUS] completed model-first 2026-07-30** — {#lane-state-machine-restatement-status} — Supersedes the ACTIVE status in `{#lane-state-machine-restatement}` above. `CaRefLaneCore.tla` defines the six-state ownership contract, `CaRelinkLaneComposition.tla` consumes its `Ready`-only certification boundary, and `RefLaneState` implements it. Both TLC batteries pass; see `docs/superpowers/specs/2026-07-30-cas-ref-lane-state-machine.md` and `docs/superpowers/models/CaRefLaneCore_RESULTS.md`.
- **[PART-WRITE-RELEASE-SEAM] the PartWriteTxn / PreparedPartWrite / receiver-guard ownership seam gets its own contract spec — USER-DIRECTED extraction 2026-07-29** — {#part-write-release-seam} — the relink redesign's r3-r6 review rounds kept grinding on ONE seam: three layers each with their own abort/retry, `isTerminal` overloaded ("committed OR released"), `PrecommitState` as the real fact-carrier, NINE scattered proven-no-send exits whose proof is erased into a generic `NETWORK_ERROR` (`CasRefLedger.cpp:3123`), three log sites emitting ERROR/WARNING before the last release attempt (false lines on settled-late), and no exactly-once emission contract for unproven releases. User ruled: extract into a standalone story — spec `docs/superpowers/specs/2026-07-29-cas-part-write-release-seam.md` (being written by the relink-design agent as the redirected v9 deliverable); relink §6.5 shrinks to a reference. The seam contract is relink-INDEPENDENT prerequisite plumbing: single-`attempted`-bit proof channel (candidate), destructor-owned last-word emission (`CasPrecommitReleaseUnproven`), severity ladder (intermediates INFO per the user's transient-state ruling), marker-sync fix (user-approved). Implementation lands BEFORE relink implementation.

**Consumer list and the asymmetry that makes it worse, added 2026-07-31 by Task 4-C's implementer while
proving an unrelated fix.** This entry already named `checkRefStream` and `OrphanManifestSweep`'s
`activeManifestKeys`; two more consumers and the sharper fact were missing:

- **`Gc::rebuildBaseline`** — the disaster-recovery scan — is a third consumer.
- **`manifestStillReferenced`**, fsck's dangling-manifest re-resolution, is a fourth.
- **And `recoverRefTableDetailed` has NO `_ckpt`/arithmetic fallback at all**, unlike `checkRefStream`'s own
  walk, which does. So a namespace hidden thoroughly enough from `LIST` recovers there as an **EMPTY table**
  regardless of any universe fix upstream, and damage under it is missed by that check specifically. That
  asymmetry — one path has an arithmetic ground and the other does not — is why supplementing the universe
  additively (Important C) does not reach it.

Found the honest way: a test written to prove the universe fix with a genuinely dangling finding FAILED, and
the reason was this, not the fix. The test was then narrowed to assert only what the fix does.
- **[CKPT-DAMAGE-NO-REPAIR-PATH] a damaged `_ckpt` has no repair path and still shuts the round-wide destructive gate** — {#ckpt-damage-no-repair-path} — from the I1 fix work (final review): the fix (`e337bb2c87d`) converts an undecodable `_ckpt` from a pool-wide FOLDING halt into a per-namespace hold (`BodyUndecodable` precedent, cursor rides verbatim, other namespaces fold; WARNING names namespace+key) — but TWO residuals stand: (a) a held namespace still shuts the ROUND-WIDE destructive gate, so one unrepaired `_ckpt` stops all reclamation pool-wide until repaired — full isolation needs Stage B's per-namespace destructive gate (the comment at `CasGc.cpp:2515` names the set the flip must carry); (b) there is NO repair path — `publishCkpt` read-merge-CASes the same object and hits the same decode failure, so the namespace's own writer is stuck too; candidate fixes = `fsck --repair` recreating from the fold contribution, or a `publishCkpt` recreate-on-undecodable arm (PROTOCOL-ADJACENT — user consult; fail-close question: recreating a `_ckpt` must never lower the frontier proof). Until then the operator action for a damaged `_ckpt` is manual object surgery.
- **RESOLVED 2026-07-30 (Stage B Task 3): [CKPT-FAILED-BIRTH-DEBRIS] a birth that never lands leaves an unreclaimable `_ckpt` that later blocks decommission** — {#ckpt-failed-birth-debris} — STAGE-B LIFECYCLE OWNER (final-review finding M3): `commitRefChunk` publishes the birth `_ckpt` before the durable PUT (ordering correct per spec §3), but if the birth never lands the `_ckpt` sits under a never-born namespace with NO reclaimer — namespace cleanup runs only for REMOVED namespaces, and the backstop's own comment records that a leaked `_ckpt` makes a drained server root refuse decommission (`claimOwnerOrThrow` → `CORRUPTED_DATA`). Fail-closed (refusal, not loss), but permanent debris. **Closed**: `CasRefLedger::commitRefChunk` now reclaims the birth's `_ckpt` (best-effort, never-throws HEAD+`deleteExact`) on the three failure branches that PROVE this attempt's ref-log bytes never landed and never can (attempt-arm race, occupant-unreadable, `SuccessorSeal`, genuine foreign interference) — deliberately NOT on the ambiguous-exception branch, where the write might still have landed. Red-first: `gtest_cas_ref_wedge_every_attempt.cpp`'s `BirthCkptIsReclaimedWhenTheGenesisTransactionIsConclusivelyRejected` (genesis birth, successor-seal conflict) failed with the `_ckpt` still present before the fix, passes after.
- **[GC-DEFER-DECISION-LIST-COST] the round's WHETHER-to-fold decision costs a full pool LIST — 79.11% of all GC time** — {#gc-defer-decision-list-cost} — TRACK-B HEADLINE (from the audit addendum `cb11e6e9501`, T15 re-validation specimen): with rounds now bounded and phase rows finally emitted, the leader is STILL busy 90.5% of wall, and 79.11% (5,096.4 s of 6,443.4 s) is `defer_decision` — a full ~177k-key LIST every round at ~465 ms/page, sometimes concluding `changed_shards:0` after 127 s. Control case is decisive: the four pre-workload Deferred rounds with `ref_keys_listed:0` cost 744-829 µs — five orders of magnitude less; the whole phase is the LIST. `ref_list_probe` used to compound it (re-listing the same keys on probe rounds); Task T5 deleted that detector outright, so this specimen's `ref_list_probe` cost no longer recurs, but `defer_decision`'s own LIST — the headline here — is unaffected by that deletion. The frozen-tail design (user-ruled) REQUIRES a LIST to discover tails, so any fix is design work, not a tweak: candidates to brainstorm = scoped per-namespace LIST with start-after markers, tail discovery from the previous round's coverage + delta probing, LIST-page caching keyed by namespace tail. PROTOCOL-ADJACENT — user consult before any change to what the round reads. Related smaller item from the same addendum: follower per-round lease check regressed 6.77→53.5 ms (7.9×, 38 s per 2 h total — note-not-alarm, cause unexplained by frozen-tail; find it while in there). Specimen-reading caveat: every predown trace dump BEFORE 2026-07-29 (`52f110e94b3`) carries an unfiltered mix of ALL trace types in both trace_CPU_*/trace_Real_* files (alias-shadowing bug, fixed) — re-derive nothing from those files without the mixed-population caveat; on-CPU stacks are double-counted there.
- **[GC-FULL-TIME-ACCOUNTING] every millisecond of a GC round must be attributed — expanded from ROUND-DURATION-ALARM by user directive 2026-07-29** — {#round-duration-alarm} — TRACK-B ITEM (was SMALL; user: "я хочу знать/измеренно видеть точно где тормозим и насколько") — TWO measured blindnesses: (1) a 41.5-minute never-completing round was invisible (heartbeat pulsing; `runRoundLogged` emits Start+Finish only — no Finish row, nothing wrong in the table); (2) COMPLETED 5-24-minute rounds show every phase timer < 100 ms — the minutes live OUTSIDE every timed span (un-wrapped fold-walk sections, meta-pool waits, in-round lease/token-stability waits, retry sleeps — exact set unknown, which is the point). FIX (three parts): (a) close the timer coverage — audit `runRegularRound` + `fold` for every un-timed span and wrap each in a phase or sub-span metric; (b) SELF-CHECK: emit `unaccounted_ms` on the Finish row (round wall minus sum of phase durations) so any future instrumentation gap is a VISIBLE number, not a mystery — target ~0, alert-worthy when material; per-phase `profile_events` already ride `GcPhaseRecord`, so per-phase S3-request counts fall out for free once coverage is complete; (c) keep the original alarm: periodic in-round PROGRESS ROW (phase + elapsed + logs folded) + WARN past a sane bound. Observability only, no control-flow. STATUS UPDATE (audit addendum `cb11e6e9501`, re-validation specimen): part (a) is MEASURED 99.986% complete already — 6,443.4 s of GC wall vs 6,442.5 s of phase rows, 0.9 s unaccounted, and the un-timed spans are exactly THREE, all sub-20 ms (`lease`->`heartbeat_floor` 68×2.1 ms; post-`orphan_sweep` epilogue 63×11.1 ms — the only one worth a named phase; pre-`pending_deletes` 3×2 ms). Remaining work: name the epilogue phase, add the `unaccounted_ms` self-check column (b), and the progress-row alarm (c).

- **[POOL-REFUSAL-NODE-FATAL] a pool bootstrap refusal takes the whole node down** — {#pool-refusal-node-fatal} — DESIGN QUESTION (2026-07-29, surfaced by the W3 RCA; pre-existing bootstrap behaviour, NOT Stage A) — the residual-data guard (`CasPool.cpp` ~:439, Code 668 `missing _pool_meta over a non-empty pool prefix`) raises during metadata loading and propagates out, so the SERVER EXITS (container exit 156) instead of starting with that one disk marked unusable. Refusing the pool is right (fail-close); taking the node down for one residual CA prefix is the question — a node may serve many disks/tables that are healthy. Direction: bootstrap-refusal -> disk marked broken/read-refused + loud diagnostics + the node UP, consistent with the disk-lifecycle redesign goals (UNMOUNT ejects, FSCK not dormant-only); the refusal message already names the operator verbs (recreate or restore `_pool_meta`). Evidence: S43's W3 answer (refusal + causation control), `2026-07-28-stage-a-RESULTS.md` row 14.

- **[FOLDED-TOKEN-VESTIGE] `ShardCoverage::folded_token` is persisted, codec-round-tripped, operator-rendered — and NEVER populated** — {#folded-token-vestige} — VESTIGE/DECIDE (2026-07-29, found by T15's polish round while adding the verbatim-carry assertion) — `Gc::fold` never assigns it; the codec writes/reads it (`CasFoldSealFormat.cpp` ~:155/:303), `CasInspect` renders it (~:371), the struct documents it as "the manifest token observed when the entry was processed" — every row carries the default `Token`. The carry test asserts it verbatim WITH an in-place vacuous-today comment (goes live the day a producer exists). USER RULED (2026-07-29): DELETE. Origin traced: the old root-shard `discover` token-diff skip (4facf0bc9e1/43cd5eef11e era) — change detection via store tokens, fully subsumed by INV-1 tail-vs-cursor contiguity; producer AND consumer died with the discover phase; even the doc comment drifted (root-shard token -> described as manifest token). Deletion task dispatched (a16, edit-now/build-after-soak); removes field + codec keys + CasInspect render + the vacuous carry assert + the orphaned round-trip test.

- **[SEAL-DECODE-REMAINING-FIELDS] the rest of the silently-defaulting-field family in the fold-seal codec** — {#seal-decode-remaining-fields} — SMALL FOLLOW-UP (2026-07-29, T16 concern 2, deliberately left out to keep the F1 diff reviewable) — `btr` missing `key`/`ck` and `cnd` missing `shard` default silently exactly the way `cls` did before T16's fix; same treatment owed (required-field refusal, CORRUPTED_DATA). One small task, same test file, after T16 merges.

- **[LEASE-BLIP-PART-CHECK-COLLAPSE] the part-check thread collapses transient CA unavailability into part corruption** — {#lease-blip-part-check-collapse} — PRODUCT FINDING, STAGE-GATING until RCA'd (2026-07-29, T15 re-validation criterion-4 catch; evidence `build/t14_revalidation/new_error_class.txt`) — `ReplicatedMergeTreePartCheckThread::checkPartImpl` consumes the CA disk's fail-close 668 (`mount lease not held; backing may be temporarily unreachable; retry once the disk recovers to Live`) as "Part ... looks broken. Removing it and will try to fetch" (13x ch1 + 14x ch2; per-minute correlation exact: broken-part events occur ONLY while the lease is not held). "Unreadable right now" and "corrupt" are different facts; the remediation is destructive-shaped (remove + refetch). Self-healed here (healthy peer held every part; both replicas 1,033,813 rows) — the worrying shapes are simultaneous double-blip (no healthy source) and single-replica pools (none by construction). NOT established: pre-existence (absent from soak-1's log, but soak-1 never reached a lease-loss window — coverage, not behaviour) and the lease-loss REASON (the 668s are the checker's consumption of not-Live, not the keeper's renewal-failure log). RCA in flight: (i) keeper-side renewal-failure reason in the blip windows; (ii) does checkPartImpl have ANY transient/retryable class today and was CA's 668 ever mapped to it; (iii) the remediation's precise loss boundary on CA disks (detached_broken preserved vs removed). FIX DIRECTIONS: map CA lease-not-held/not-Live to the check thread's retry-later path (minimal upstream surface — an exception-class mapping, not new machinery); or gate part checks on disk lifecycle state (relates to `{#pool-refusal-node-fatal}` and the disk-lifecycle redesign goals — the same "transient unavailability must not look like damage" family). RCA COMPLETE (2026-07-29 evening, `build/t14_revalidation/rca_lease_blip_part_check.md`): PRE-EXISTING — the throw site blames to `21d207734095` (2026-07-23, six days before the T15/T16 merge); the re-validation produced the CONDITIONS (lease blip under checkpoint fsck load), not the collapse. Scale corrected: ONE part in a ~5s retry loop (15+21 events), not 27 parts. MECHANISM: `ReplicatedMergeTreePartCheckThread` already has the retryable escape hatch (`isRetryableException` rethrow at `:398`) but `isRetryableException` (`checkDataPart.cpp:70`) does not list `INVALID_STATE` — the exact code the CA disk raises for a transient lease gap whose message says "retry once the disk recovers to Live". TRIGGER (keeper's own words): `background renewal failed transiently ... Code: 499/1000` then `the mount-lease stops advancing`, remount after ~36.5s token-stability wait — renewal I/O timed out during the checkpoint window; the fsck-load-on-single-rustfs hypothesis is CONSISTENT, labeled inference. LOSS BOUNDARY: detach-not-delete (`TryFetchMissing` detaches, removes from ZK, queues a fetch); worst case = availability + manual ATTACH on double-blip/single-replica, not silent loss. CHARTERED FIX (de-escalated from stage-gating): classify the CA transient-unavailability as retryable so the EXISTING hatch fires — design note: prefer a precise mapping (dedicated error code or code+context guard) over blanket `INVALID_STATE` retryability (that code is broad); red-first test = the INDUCED-BLIP mini-run (S13 keeper-starve + forced CHECK TABLE in the window, fixed predown list, live three-table queries) which ALSO completes the open ref-plane question (did remove-broken drop CAS refs; did re-publish follow — part_log/CA-event-log were lost with the cluster; predown_dump.sh now captures both, `fec36da03c3`). Stage disposition: row 12d = documented pre-existing finding w/ chartered fix (relink-storm precedent); verdict flipped to PASS (`3f7b35c7ce1`). INDUCED-BLIP RUN DONE (RCA appendix, `25ca4cffc7d`, 6 min, first-try repro via `docker pause` rustfs + `CHECK TABLE` in window): (a) REF-NEUTRAL — zero CA-log events at the detach, no ref drop, nothing dangling, no re-publish owed; (b) the detach NEVER EXECUTES under this interleaving — the destructive move needs the same not-Live disk that triggered it, so "Removing it" removes nothing (part stayed ACTIVE, no `RemovePart` row, empty `detached/`), which also explains the 5 s announce loop; (c) residual = recovery between decision and move (double-blip shape) could complete the detach ⇒ the fix stands, RE-CLASSIFIED robustness-not-data-safety; (d) predown fix proven — both tables in the dump AND queried live. IMPLEMENTED 2026-07-29 late evening: `58578af0c6d` (helper + 3 routed sites + contract split + carry-alongs, both gates green) + polish `f769b19d7fe` (suffix promises only what the site can prove; scope contract reconciled; d3 pinned; exceptionOf sentinel); scoped review APPROVED (pin verified genuine, FLAG-1 fail-close confirmed). **LIVE-VALIDATED 2026-07-29 21:11-21:16 — THE COLLAPSE IS GONE** (validation section in `build/t14_revalidation/rca_lease_blip_part_check.md`, artifacts `build/t14_revalidation/validation/`): same runbook, same part, binary rebuilt 21:09:21 (postdates both fix commits). Anchored predicate `message LIKE 'Part %looks broken%'` = **0 on both nodes**, `Detaching it, removing from ZooKeeper` = 0, `INVALID_STATE` never raised; instead `checkDataPart` logs `Debug` "Got retriable error … Code: 210 … TRANSIENT unavailability, not damage" and the thread reports "**Part all_0_5_1 looks good.**" Part stays Active, 6000 rows both replicas, `detached_parts` 0, no `RemovePart` row, ref plane unmoved. Window proven open independently (renewal "stops advancing" 19:12:00 → `mount_remount` 19:15:23). Negative control = the pre-fix announcement preserved by the predown fix at `logs/predown/ch1/induced_blip/text_log_error_shapes.tsv`. THREE NOTES FROM THE RUN: (1) the hatch that fires is the INNER one — `checkDataPart.cpp:503`/`:536` returns empty checksums, so `:398`'s rethrow is never reached (same classifier, same outcome, different mechanism than the charter stated); (2) consequently `CHECK TABLE` now answers `1` (not-broken) for a part it did NOT verify — upstream's documented retryable behaviour (`checkDataPart.cpp:509`), so CA is now consistent rather than special, but it IS a change in what an operator sees during a blip — RULED ACCEPTABLE (controller, 2026-07-29): uniformity with upstream's documented retryable semantics is the point of direction (ii); the check re-runs on next read, and terminal conditions still fail loudly; (3) non-alarming severity holds at the part-check site (`Debug`) but not universally — `IMergeTreeCleanupThread` still logs the transient at `Error`, as it did pre-fix with 668. Fix design AUDITED (2026-07-29 evening, `docs/superpowers/reports/2026-07-29-ca-transient-classifier-audit.md`): plane separation shows exactly TWO destructive-misclassification sites tree-wide (READ plane: `ContentAddressedMetadataStorage.cpp:1111` TransientNotLive + `:1163` probe Indeterminate); 50 of ~58 CA transients already throw upstream-retryable codes (32 NETWORK_ERROR + 18 ABORTED). USER-APPROVED 2026-07-29 evening — direction (ii) — `throwCasTransientUnavailable` helper minting NETWORK_ERROR, routed at the 2 destructive sites + `CasMountRuntime.cpp:106` for Write-plane uniformity; ZERO upstream edits; cost = lease gaps share a `system.errors` row with socket errors. Direction (i) rejected 9-wrong-to-1-right (18 unintended reclassifications incl. KeeperMap/NATS). Separate filings from the audit: `CasGc.cpp:718` ABORTED-on-write-once (unbounded GC retry, should be CORRUPTED_DATA); `CasBlobInDegree.cpp:289` deposed-leader absence-as-corruption window; `CasServerRoot.cpp:241/:261` ErrorCode mistags; SECOND Read-plane hole = `UNKNOWN_FORMAT_VERSION` version-skew detaches parts during rolling upgrade (benign pre-release, track before posture change). **RCA 2026-07-29 (`build/t14_revalidation/rca_lease_blip_part_check.md`)**: ONE part, not 27 — the count was a ~5 s re-check loop over 3.5 min. MECHANISM: `ReplicatedMergeTreePartCheckThread` already rethrows on `isRetryableException`, but that classifier (`checkDataPart.cpp:70`) omits `INVALID_STATE`, which is precisely what the CA disk raises for a transient lease gap (`ContentAddressedMetadataStorage.cpp:1112`) in a message that says "retry once the disk recovers to Live". PRE-EXISTING: `git blame` dates the throw site to `21d207734095`, 2026-07-23, six days before the T15/T16 merge. TRIGGER: keeper renewal timed out (Code 499 / Poco 1000) twice while the lease was valid, then stopped advancing; re-mounted after a ~36.5 s token-stability wait — consistent with fsck load starving renewal I/O against single-instance rustfs, though that attribution is inference. BLAST RADIUS: part is DETACHED (bytes preserved), removed from ZK, fetch queued; worst case is double-blip or single-replica, where the fetch has no source and the part is missing until manual ATTACH — availability + manual recovery, not silent loss. MINIMUM FIX: classify the CA transient-unavailability error as retryable so the existing escape hatch fires. STILL OPEN: whether the remove-broken path drops CAS refs and whether a re-publish follows — `part_log`/`content_addressed_log` died with the container (now added to the predown dump, `fec36da03c3`). **INDUCED-BLIP RUN 2026-07-29 (appendix in `build/t14_revalidation/rca_lease_blip_part_check.md`)**: reproduced deterministically by pausing rustfs to starve lease renewal past the TTL, with `CHECK TABLE` forcing the consumption. TWO FINDINGS THAT LOWER SEVERITY. (1) REF-NEUTRAL: `system.content_addressed_log` shows ZERO ref-plane events at the detach — the last `root_remove` is 2.5 min earlier and belongs to the merge — so the remove-broken path drops no CAS refs, dangles nothing, and has no re-publish to await. (2) THE DETACH DOES NOT EXECUTE: `detached_parts` empty afterwards, the part still Active, both replicas at 6000 rows, no `RemovePart` in `part_log` — the destructive step is gated by the same not-Live condition that triggered it, because the move would touch the unavailable disk. So the ERROR line says "Removing it" and nothing is removed. RESIDUAL RISK, not waved away: this is the behaviour at this timing; a disk recovering between the decision and the move could let the detach through, which is the double-blip shape. The `isRetryableException` fix removes the question by never reaching the decision. **FIX LANDED (2026-07-29)** — direction (ii) as approved: `throwCasTransientUnavailable` (`Backend/CasRequestControl.{h,cpp}`, beside `throwCasWriteRetryLater`) mints `NETWORK_ERROR` with a message that names the CA condition AND classifies it transient; routed at the two destructive READ-plane sites (`checkOpAdmitted`'s `TransientNotLive` arm and `confirmPoolIdentityForEmptyEnumeration`'s probe-`Indeterminate`/`AccessDenied` arm) plus `CasMountRuntime::checkFenceOrThrow` for Write-plane uniformity. ZERO upstream edits. The shared-code contract `gtest_cas_operation_gate.cpp` is SPLIT — `TransientNotLive` ⇒ retryable, `IdentityLost` and every terminal lifecycle throw ⇒ unchanged 668 — and a new gate test asserts the outcome against upstream's own `isRetryableException` rather than a code number.

- **[cas-format-version-floor] `checkCompatibility` rejects only versions ABOVE `G_BUILD`, never below a type's own birth generation** — {#cas-format-version-floor} — found 2026-08-04 while ruling on the empty-universe GC gate fix (`FINDING #3`, emptied pool stops reclaiming) — `checkCompatibility` (`Formats/CasFormat.cpp`) throws `UNKNOWN_FORMAT_VERSION` for `compatibility_version > G_BUILD` but accepts anything below it, including a version under the type's own birth generation (`changePoints(id).front().generation`, e.g. `RefCatalog` born at generation 4 — a header claiming generation 1 decodes as if legal). `decodeRefCatalog` also discards the parsed `TextHeader` once the check passes, so nothing downstream can recover the version even for logging. Net effect: "decoded successfully" does not today imply "carries a legal version for this type". NOT required by the empty-universe gate: that proof rests on token-present + a full structural decode (type, complete records, matching count trailer, no trailing bytes) + zero entries across every lifecycle state, and a well-formed-but-out-of-protocol empty catalog is already an accepted residual under the trusted-store model (a token proves byte identity, not history) — closing this floor would only shrink that residual, not remove it. FIX DIRECTION: a per-type birth-generation floor enforced centrally in `checkCompatibility` (it already has `FormatId` at every call site via `expectHeaderLine`/`expectRunHeaderLine`/the blob envelope decoder), refusing a version below `changePoints(id).front().generation` as `CORRUPTED_DATA`. Blast radius is the shared format layer (used by every CAS object type) — needs its own failing-first coverage and an audit of existing fixtures/artifacts that might carry a low version, so it is deliberately out of scope for the gate fix and tracked here instead. If the accepted fault model ever needs to close the well-formed-illicit-empty-catalog residual too, that is a DIFFERENT, wider feature (a monotonic catalog epoch/generation or chained digest committed into durable GC state) — this floor alone would not close it.

## 4. Read / write path {#read-write}

- **[ckpt-read-policy] modular `_ckpt` first-attempt view: conservative / cached / prefetch** — DESIRABLE (write-path; noted 2026-08-03, USER-DIRECTED design shape 2026-08-03: `_ckpt` handling must be modular/replaceable) — {#ckpt-read-policy} — Cost being addressed: since INV-4 landed (`eeadbc1887e`, Stage A), every committed ref-log chunk pays `GET _ckpt` + token-CAS *serially after* the log `PUT` (spec `2026-07-27-cas-ref-chain-complete-cut-design.md`); the two ref transactions of one part (precommit + commit) are serial phases and never share a chunk, so a **lone INSERT pays +4 serial RTTs** (2 GETs + 2 CASes); the pre-INV-1 id allocator was an in-memory `fetch_add`, i.e. zero S3, so nothing offsets this, and batching amortizes across concurrent parts only — the cost concentrates on low-concurrency insert latency. DESIGN SHAPE (user): a pluggable policy choosing only **where `publishCkpt`'s FIRST attempt gets its `{body, token}` view** — the invariant core is shared and policy-independent: retry-after-conflict ALWAYS does the whole-body exact re-read + re-merge (the "read the WHOLE body every attempt" rule in `Pool/CasRefCkpt.cpp` is about exactly this arm and stays), `lifeEpochWouldDecrease` runs against the view and again after any re-read (a stale token cannot win against a changed object, so detection defers at most one conflict round), and the durability order `log PUT → _ckpt CAS → ack` is untouched by every policy. Policies: (1) **conservative** = today: fresh GET per publish; (2) **cached** = seed with one GET on first touch, then serve the view from the writer's own last winning CAS (`RefTableRuntime`-scoped) and go straight to PUT-if-match — expected to essentially ALWAYS hit, because every `_ckpt` author of a live life (lane writer, snapshot publisher, recovery, genesis `completeCreation`) lives in the lease-owning process and cross-process writers are excluded by lease exclusivity; a miss is therefore a SIGNAL (fence loss / remount / foreign recovery), worth its own ProfileEvent, not noise; (3) **prefetch** = one paginated `LIST` over `cas/ns/state/` at mount seeds all `_ckpt` views, then memory-only + PUT-if-match — LIST here is a pure HINT and does NOT re-enter the trust model: an omitted/stale row just costs one lost CAS or one lazy GET on first touch, correctness rides the conditional write, consistent with LIST's demoted role. Mandatory cache-invalidation edges for (2)/(3): fence-generation change, wedge, remount supersession, `catalog_life_invalidated`. OUT OF SCOPE for the policy seam — always exact reads: recovery's `_ckpt` sample (memory is the thing under suspicion there) and GC-fold's frontier GET (possibly another process; needs the durable fact). Effect: +2 → +1 RTT per chunk, +4 → +2 per lone insert; the remaining CAS is irreducible (it IS the frontier mechanism, `NoAckedLoss`). MEASUREMENT PRECONDITION: the stage-1 figure **1.59x predates `_ckpt`** (measured 2026-07-24; `_ckpt` landed 2026-07-28) — re-run the wide-insert baseline on current HEAD first, then bench per policy against it. Per [[feedback_head_before_put_protocol_untouchable]]: no protocol step removed, nothing reordered in the durability chain, but it touches the commit path — ships as an explicit reviewed decision with before/after numbers; default policy choice (likely conservative until soak-proven) is part of that decision.
- **[write-path stage 1] parallel intra-part blob upload — LANDED (2026-07-24)** — The single-threaded serial blob-upload bottleneck of the wide CAS-on-S3 INSERT (documented in [[project_cas_insert_slowness_writepath]] and the point-4 baseline `docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md`) is now addressed by write-path stage 1: a server-wide `CasBlobUploadPool` + `fanOutBlobUploads` fan out a part's blob PUTs/dedup-HEADs (spec `docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md`, plan `docs/superpowers/plans/2026-07-23-cas-writepath-stage1.md`). Re-profile (`docs/superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect.md`): CA wide-insert wall **58.41s → 30.26s**, CA-vs-plain **3.0x → 1.59x**, top-thread Real share **72.3% → 14.5%**, CPU/wall **0.375 → 1.075** — the single-threaded signature is gone. Residual gap = the serial cross-part commit (ref batch still 1.0 BY DESIGN → **stage 2**, concurrent `commitPart` dispatch) plus the CAS-only dedup HEAD/GET traffic (~12% of Real wall — see `[B121 / B202 / one-GET-open]` below and the HEAD-before-PUT gate). Stage 2 is the next lever, now scoped against a smaller residual than the original 3.0x framing.
- **[TXN-ONE-PIPELINE] complete the "staging ops never defer" invariant** — HARD (small, structural) — The `01603` column-TTL abort (`de8a38b1e87`) was an ordering inversion between `DiskObjectStorageTransaction`'s two dispatch pipelines: eager (CA staging ops: `writeFile`/`createHardLink`/`moveDirectory`, and now part-file unlinks) vs deferred-to-commit (durable ops). A total order is impossible (read-your-writes B58/B63 and B151 force pre-commit effects; abort safety forces commit-gated durable deletes), so the correct invariant is *per-state-domain*: EVERY op that touches in-memory part staging must dispatch eagerly, leaving the deferred queue exclusively for durable non-staging effects (the two domains commute, closing the inversion class structurally). Residual gaps: `moveFile`/`replaceFile` in the part-file→part-file shape are still deferred (their B182 eager hook was deleted in T6 as trigger-less — the same "no trigger today" state `01603` was in before T8). Target shape (user direction 2026-07-15, option B): the deferred queue CEASES TO EXIST for CA — durable effects become staged INTENTS too (verbatim-file delete intent, part ref-drop intent, mountpoint delete intent) materialized at commit, and CA gets its own `ContentAddressedDiskTransaction` subclass dispatching every op straight to the metadata transaction in program order, deleting all four per-method `isContentAddressed()` branches from the base class (each of which was added through a bug: writeFile, createHardLink B58/B63, moveDirectory B151, unlink gate `de8a38b1e87`/`725dbc7d83c`). Delicate parts: verbatim writes are durable-immediate today (staged-write unification touches the mutation-MVCC read-modify-rewrite append) and autocommit one-shots keep their individually-durable contract. REFINED (user, 2026-07-15 evening): pair it with an explicit TWO-PHASE disk-transaction contract — new `IDiskTransaction::precommit()` (noop by default, zero change for ordinary disks); CA precommit = the ENTIRE publish (manifest from the overlay -> `precommitAdd` -> upload missing blobs -> promote), called before ZK-multi in Replicated (next to `renameParts()`) and before the `data_parts` lock in plain `Transaction::commit`; CA commit = durable-intent materialization only; abort-after-precommit = today's `dropRefBestEffort` compensation in an explicit phase. `moveDirectory` stops publishing (pure staging re-key) — B151's rename-window publish and the `rename_published_refs` machinery are DELETED. Verified basis: parts are `PreActive` until disk commit (no owner reads), the fetch handler serves `PreActive` but vanilla plain-s3 already fails+retries such fetches (queued rename), so the announced-but-unpublished window is an accepted upstream race, NOT a protocol obligation — the earlier 'publish must precede ZK announce' rationale was overstated. Contract decisions (user-settled 2026-07-15): `commit` IMPLICITLY runs `precommit` when it was not called (idempotent, flag-guarded — classic commit-implies-prepare; an assert would turn a positioning omission into a prod abort across dozens of commit call sites incl. the Keeper-recovery branch), WITH observability (ProfileEvent `CasImplicitPrecommitInCommit` + debug log) so a mis-positioned hot path surfaces in soak metrics; staging mutations arriving AFTER `precommit` are a genuine correctness violation -> fail-loud `LOGICAL_ERROR`. SCOPE ADDITION (user 2026-07-15): the refactoring must also DE-PATCH upstream code — remove the accumulated workarounds around eager-dispatch/read-your-writes in non-CA files; inventory with A/B/C classification + de-patching order = `docs/superpowers/cas/upstream-patch-inventory.md`. SPEC: `docs/superpowers/specs/2026-07-15-cas-txn-one-pipeline-design.md` (2026-07-15, approved brainstorm) — SUPERSEDES the staged-intents wording above: everything-immediate model (no intents — ABA rejection; local-disk abort semantics for verbatim/mountpoint deletes), single `dispatch` funnel in `DiskObjectStorageTransaction` (no CA subclass), generic `tryCreateWriteBuffer` hook, `precommit` call sites in `renameParts`/plain `commit`/freeze/restore/fetch; lands BEFORE codecs v3 and the source-layout refactoring.
- **[RELINK-CONFIRM-BUSY-LANE] fetch-by-relink is ~17% available under sustained writes** — {#relink-confirm-busy-lane} — REAL PRODUCT FINDING (2026-07-29 Stage A T14 soak, pre-existing behavior — NOT a Stage A regression: `confirmExactRef` refusal logic verified untouched by the stage, comment-only hunk) — `CasRefLedger::confirmExactRef` rule 3 (`CasRefLedger.cpp` ~:445) refuses `Unknown` while `wedge || !pending.empty() || leader_active`; a busy writer's lane is essentially never quiescent (sender at 168,955 `CasRefBatchFlushes`: ~92k refusals vs 19,531 proven ≈ 17% availability), so the receiver at `DataPartsExchange.cpp` ~:1550 abandons the relink with an ERROR-severity `NETWORK_ERROR` (~1000-3700/min, ~106k rows per 20 soak minutes, zero correctness consequence — replication converges, replica row counts equal). Fail-closed and CORRECT per the trust model; the finding is cost+noise+observability: (a) ANSWERED (2026-07-29, code read at the throw site): the abandon THROWS NETWORK_ERROR = the retry-later class — the replication queue stores the fetch, backs off, and RE-SELECTS on re-execution; a byte re-request to the same source is explicitly forbidden as unsound (comment at the row-3 throw). So refusals cost replication LATENCY + noise, never duplicate bytes and never double-publish; residual quantification = convergence-delay distribution under sustained writes; (b) USER-DIRECTED (2026-07-29): an `Unknown` that is OUR OWN uncertainty and part of the protocol must not report Error — demote severity AND rewrite the message text to name a TRANSIENT state explicitly (e.g. "relink confirm returned Unknown — an expected, transient outcome while the source lane is busy; the fetch is re-queued"), + ProfileEvents pair (proven/refused) + sender logs WHICH rule refused; AND the unhappy path gets its own PRECISE TEST — deliberately drive a refusal (pending item on the target name / wedge / fence-lost), assert severity + message class + no byte re-request to the same source + eventual convergence of the retry cycle; (c) the ledger's `Unknown` maps SILENTLY through `ContentAddressedMetadataStorage.cpp` ~:1996 — surface WHICH rule refused (took a live cluster to diagnose; a log grep should have sufficed); (d) design pass USER-APPROVED and DECIDED 2026-07-29: variant (ii) RE-OFFER chosen, the per-ref MutationScope index RETIRED pre-ship (see draft §0 for the full decision set: mandatory-recovery/discretionary-maintenance split, remote-recovery BUDGET w/ own counter vs the LRU-eviction amplifier, axis (iii) rejected on who-pays, NAMED TLA assumption, taxonomy rebuilt from the new state set); draft: `2026-07-29-relink-confirm-per-ref-draft.md`; rule 3 was table-scoped — any pending mutation refuses ALL refs; a per-ref refinement (confirm from committed rows provably unaffected by the pending window) could restore availability without weakening fail-close. Functional cross-check lives in `test_cas_replicated_relink`. MEASURED at scale (gc-audit 2026-07-29, `reports/2026-07-29-gc-perf-audit-soak.md`): 248,400 refusals in ~32 min cluster-wide, peak 9,219/min on one node, each paying an ERROR-severity exception with ~23 symbolized stack frames — a CPU profile of that soak would have measured the relink storm, not GC.
- **[B121 / B202 / one-GET-open] read request-count reduction** — DESIRABLE (design pass) — B202 inline-by-size (drop the file-type predicate, inline < ~512 KiB, weigh the wide-part-medium-column regression, `.bin` carve-out) + a per-blob-GET read-cost reduction (B121) + one-GET part open (pack small files). Pure perf/request-count; no safety dimension. Companion to the (landed, opt-in) file-cache disk for re-read-heavy workloads.
- **[B98] Streaming `putOverwrite` (condemned-displacement)** — DESIRABLE — The rare INV-1 revival/displacement path still materializes the whole body; not a blocker.
- **[promote-recreate] promote-time in-place recreate of a condemned SOURCED (tokened) blob** — DESIRABLE — The tokened promote gate stays fail-closed `ABORTED`; recreate happens on the retried build via `putBlob` cold-reuse. The tokenless-evidence copy-forward case is DONE. Ideal root-cause fix (writer-triggered synchronous fold-barrier at promote) is blocked by the lack of a writer↔GC synchronous-fold API — deferred behind the landed bounded resurrect.
- **[R1/X1] ephemeral reader pin (cross-node GC fence)** — DESIRABLE / VERIFY — Per-server-owned namespaces narrow the window and a live ref resolving to an absent object surfaces `FILE_DOESNT_EXIST` (INV-NO-DANGLE), so for normal MergeTree this is covered by DataPart lifetime; the ephemeral-pin mechanism is design-only. Audit whether any ref-less/cross-node reader path exists before implementing.
- **[ch128ctx] slot-bound blob-hash middle tier** — DESIRABLE (small spec) — New `BlobHashAlgo` variant: `cityHash128(content) ∥ xxh3_64(part_name, file_name) ∥ size` (256-bit; variable-width `BlobDigest` already supports it). Binds blob identity to its minting slot, so *cross-slot* collisions (the realistic adversarial dedup vector: attacker-crafted content deduped into a victim's future blob) become useless, at ~zero CPU cost over `cityHash128`. Every load-bearing dedup survives: relink/carry-forward are reference-based; retry idempotency, same-name replica writes, and snapshot-upload→TTL-move prepayment are same-slot; only cross-slot content coincidence is lost (an explicit non-goal, `01 §what-it-does-not-buy`). Middle tier of `cityHash128` → `ch128ctx` → `sha256`. Main touch: the hasher interface needs `(part_name, file_name)` context injection into `putBlob`. Origin: backup manifest-reuse discussion, `10-backups.md §multi-disk` (2026-07-14).
- **[codex-26] `casAppendObject` needed before any concurrent appender** — LOW (latent) — `CasPlainObjects::casPutObject`'s CAS loop re-reads only the TOKEN on conflict, retrying with the SAME frozen `bytes` payload the caller froze at buffer-open (`ContentAddressedTransaction::writeFile`'s Append branch) — a fresh-token/stale-payload lost-update shape (2026-07-17 codex-review triage, finding №26). Not reachable today: the only production appender is the mutation-entry CSN write (`MergeTreeMutationEntry::writeCSN`), one append per mutation-unique key under the per-table single-writer lease, so there is no second appender to lose. Required before any future concurrent appender lands: a real `casAppendObject` that re-reads the base content (not just the token) inside the retry loop. See the single-appender invariant comment at `casPutObject`.

## 5. Staging / adoption {#staging}

- **[out-of-band staging adoption] adopt bulk-load/backup/external-tooling uploads via verified copy-forward** — HARD (needs spec) — Distinct from the landed (opt-in) S3-native writer staging. Objects uploaded out-of-band land under a staging prefix and are ADOPTED into the pool via the verified hash-then-publish copy-forward path instead of being trusted in place. Scope/semantics to be specced.
- **[S3-native staging §7] memory fast-path for small blobs** — MINOR (optional) — Buffer sub-single-part blobs in memory and `putIfAbsentStream` (no disk/staging/copy).

## 6. Backends — real-store validation, GCS, LIST consistency {#backends}

- **[GATE #1: Azure] real-store GC validation on Azure** — GATE — AWS + GCS DONE (2026-07-03, live-validated). Azure not started — the last leg of the real-S3 reclaim release gate.
- **[GCS production-grade follow-ups]** — DESIRABLE — Compose-based conditional finalize for blobs above `gcs_max_conditional_put_bytes` (multipart silently ignores the precondition on GCS → currently throws `NOT_IMPLEMENTED`); `gcp_oauth` dialect probe validation against live GCS (ADC creds); generation-aware LIST discovery (GC re-reads every shard on GCS since list tokens are disabled — cost only); signed `x-goog-*` `extra_headers` on `gcs_hmac` (currently unsigned).
- **[LIST consistency on real S3] token-diff discovery under eventual consistency** — TEST/GATE — S3's LIST may not reflect a just-PUT key; code handles it conservatively but needs real-S3 testing. Add a LIST-consistency probe in `Cas::Probe` before LIST-derived discovery is trusted on a given store. Also load-bearing for the (moot) registry-removal LIST premise.
- **[B196] cap `s3_max_connections` to backend permits** — HARD (cheap) — CONFIRMED still open: no CA code caps `s3_max_connections`; prevents 503 + retry storm under high concurrency.
- **[F2 / rustfs#3231] false-404-under-load + overwrite-leak upstream report + repro** — INFRA — Dominant scale blocker (caps merge-heavy full-scale + 4h chaos soak). Our side is safe (clamp + destruction suppression). Needs a #3231-free/fixed rustfs or the S22 fault-proxy stand; build a repro on the #3231 dir-bloat repro.

## 7. Operability & release gates {#operability}

- **[B197] `SYSTEM` control surface — START/STOP GC, POOL READONLY, CHECK** — GATE — Product-side GC stop is currently only a soak-harness workaround.
- **[B198] backup/restore runbook** — GATE.
- **[B180 / format-freeze] pool-format version breadcrumb + first-release format freeze + rollout machinery** — GATE — Stamp the pool self-describingly; freeze the format on the first persisted-data release (schema-evolution framework is in place); durable roster + `max_content_addressable_pool_format` setting/rollout machinery not built (Part IV).
- **[B131] repo hygiene + M-W comment sweep** — GATE — 30 dangling `M-W`/`D-W1`/`2026-06-12-ca-core-m-w` comment references across 13 src files (incl. `ContentAddressedMetadataStorage.{h,cpp}`, `CasGcScheduler.h`, `DataPartsExchange.cpp:106`) reference the deleted plan — sweep to self-contained wording. Non-shippable files: `poc/cas_mergetree/` already deleted (F1 landed); the untracked empty `poc/` husk remains.
- **[B15/B99/B169/B159] `system.*` views for pool/blob/part refcounts + `clickhouse-disks` decode/introspect** — HARD (PARTIAL) — GC log + event log + `content_addressed_mounts` + ca-fsck/dryrun/rebuild/ca-inspect CLI done; per-part/ref `system.*` views + a top-down decode/traversal surface not yet. (INTROSPECTION-1/2 close signals.)
- **[B13] migration path for existing tables** — HARD — `ALTER TABLE … MOVE PARTITION` to a `content_addressed` disk re-packs; mixed-version rollout rule (read-new-before-write-new; format self-check fails closed) + a rollout-safety spec.
- **[B48 / B167a/f] clean shutdown** — RESOLVED (verified 2026-07-23) — **B48 no longer reproduces**: `clickhouse local` + CA disk (local backend, `gc_interval_sec=2`) exits cleanly on today's release AND ASan builds across all variants — immediate exit, ~9s alive with GC rounds on the dedicated thread + `OPTIMIZE FINAL` + `DROP`, exit with the table still alive (no `DROP`), and re-open of an existing pool; trace log shows active `CA GC round N` ticks and `Destroying disk content_addressed` ~34ms after the last round (repro kept in `tmp/b48_repro/`). Fixed structurally by the rev.8 disk-lifecycle round (GC-scheduler self-exit + fail-loud teardown). **B167a/f code-verified as implemented**: `Context::shutdown` (`Context.cpp:1047`) → `DiskObjectStorage::shutdown` (`DiskObjectStorage.cpp:511`) → `ContentAddressedMetadataStorage::shutdown` (`ContentAddressedMetadataStorage.cpp:834` — waits out an in-flight synchronous GC round, detaches under `pointer_mutex`, then `stop()` joins the GC worker+heartbeat threads) → last pool ref drop runs `~Pool` (`CasPool.cpp:745` — stop remount thread → bounded `drainRefLanesForShutdown` → `CasMountRuntime::finishTeardown`), which on a certified drain has the keeper terminal op stamp the lease already-expired + fold in the watermark farewell (`min_active = UINT64_MAX`, `MountRelease`/"farewell" audit event), and on an uncertified drain fail-closed skips the clean marker (successor uses observation-based reclaim). Inline `disk(...)` disks are registered via `Context::getOrCreateDisk` into the SAME selector map the shutdown loop iterates, so they are covered. Out of scope here (tracked separately): hard-kill leaves no farewell by design, and DROP-TABLE disk eject remains the disk-lifecycle-leak item.
- **[F1-prod] read-only same-pool shadow disk (`ca_ro`) breaks table load on restart** — GATE (prod) — MergeTree part discovery finds every part twice → `UNKNOWN_DISK` on restart with CA tables. Stand workaround shipped (standalone `clickhouse-disks -C` fsck-only config; propagated to the default stand); PRODUCT fix (part discovery skips `readonly` same-pool disks, or a `hidden`/`introspection_only` disk flag) still open; `10replicas`/`gc_shards2`/`awss3` server configs may still embed `ca_ro`.
- **[B165] server OOM at hour-4 soak (~49 GiB RSS)** — VERIFY — Not reproduced since the `putBlob` streaming fix; re-run a long soak to confirm resolved.
- **[B14] expedited / GDPR right-to-erasure delete** — DESIRABLE — Under GC lock, confirm no live ref, then delete bypassing the two-phase graduation delay; no layout change.
- **[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation.
- **[B26 / B135] [B66a] → §14** — local/emulated-backend items collected into §14 {#local-backend} (2026-07-23 grooming, user direction: local-backend stories live in ONE section).
- **[B66b] relink-into-detached (zero-byte `to_detached` fetch for same-pool parts)** — IN PROGRESS (2026-07-23) — folded into the publish-confirm fetch-handoff iteration (spec `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md`): relink already publishes under `tmp-fetch_<part>` and re-keys via `renameTempPartAndReplace`, so detached needs only lifting the `!to_detached` advertise gate (`DataPartsExchange.cpp:540-545`) + the detached temporary name + the same confirm step; collision semantics inherited from the byte path by construction. (RPL-4 perf cliff.)

## 8. Mount-lease / fence recovery {#mount-fence}

- **[P3.1 Task 6 / S13] live validation of fence-recovery** — TEST — TLA+ gate PASSED and the correctness paths landed (self-remount on GC fence-out is DONE); the gtest sweep + S13 3×-green live gate remain. **Task 5** (decouple renewal from the retired-view sync beat) is likely **MOOT** — freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`; confirm and close.
- **[A7-residual] gc_scheduler lifetime vs manual rounds** — VERIFY — Believed addressed by `89845c2a544` (shutdown serializes gc_scheduler teardown with health reads; wedged-lane count pinned) on top of the stabilization A7 fix. Confirm no residual: (a) a manual round on a raw pointer captured outside the lock, (b) lazy creation resurrecting a scheduler after shutdown.
- **[STID-3982-3b48 part 2] mount-lease renewal self-race on an ambiguous client-side timeout aborts the server (SIGABRT/`LOGICAL_ERROR` under ASan)** — HARD — CI-confirmed 2026-07-24 (Altinity PR #2073, run 30019911967, `Stateless tests (amd_asan_ubsan, content_addressed s3 storage, parallel)`, report SHA `0ff1cbf`; that SHA already contains both STID 3982-3b48 fixes — part 1a `8742d746d4e` "vanished mount slot stops renewal without LOGICAL_ERROR" and part 1b `cafb64652d0` "absent mount lease at clean release is a no-op" — so this is a **third, still-open variant** of the same family). Server log timeline: `23:29:53.575 <Error> CasMountLeaseKeeper: background renewal failed transiently, retrying while the lease is still valid: Code 499 ... Code 1000, e.code()=0, Timeout` (a CLIENT-side timeout on the renewal PUT — ambiguous, may have applied server-side) → `23:30:03.613 <Fatal>: Logical error: 'CAS mount-lease: key '.../mount' was touched by a foreign writer — failing closed, never re-minting'` (SIGABRT). Stack: `MountLeaseKeeper::onRenewMismatch` (`CasServerRoot.cpp:879`) falls through all three classified branches (`fenced_by_gc`, `superseded`, `foreign_writer` — none matched) into the base class's generic throw (`SingleWriterSlot::onRenewMismatch`, `CasServerRoot.cpp:1011`), which aborts debug/ASan builds at exception construction (same abort hazard part 1a/1b were written to avoid). Root cause: the timed-out renewal PUT #1 likely SUCCEEDED server-side (bumping the lease body's token/seq) despite the client not observing the ack; the soft "transient, retry" path then re-sends renewal PUT #2 with the STALE pre-timeout token, which mismatches against the (self-)bumped body; the read-back body has `server_uuid == ours`, `writer_epoch == ours`, NOT `gc_fenced` — i.e. it is provably OUR OWN live claim, but none of the three classifier cases models "differs only because of our own in-flight ambiguous retry", so it is misdiagnosed as an unclassifiable/foreign-writer collision. Fix direction: add a 4th classifier case — same-uuid + same-epoch + unfenced + token-differs — recoverable (adopt the newer token as our own successful renewal), not fatal; or make the renewal PUT idempotent/replay-safe (e.g. carry a client-generated request token so a retried request that already landed is recognized rather than conflicting with itself). Also: `amd_msan`/`amd_tsan` CAS-s3 stateless jobs in the SAME run hit the 6h hard job timeout with zero artifacts (no `result_*.json`, no logs, even `gh run view --log` truncates ~seconds into the run) — unknown whether they hit the same crash in a restart loop or hung some other way; see the CI observability-gap note (`reference_cas_ci_observability_gaps.md` #6, a project-memory doc, not yet ported into this backlog's doc set) for the "6h hang leaves no forensic trail" gap this also exposed. **FIXED 2026-07-24** — landed as the fence-not-rescue redesign, spec `specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md` rev.4, now Status `IMPLEMENTED`: TLA+ gate `8451222bb14` + `f39f2070bbd`; Phase A classifier `2e5b2df7397` + e2e test `07c8770eb0b`; Phase B keeper anchoring `e6b1d90acc0`, startup-arm `e0ee7af7564` + remount-arm addendum `25e3e34413c` + regression test `683579789c7`; Phase C guard `6094c1473ea` + follow-up `d00cc114af8`. Full `Cas*:CA*` gtest gate green post-landing. Two items from this entry's scope stay open, spec-noted: (a) the `amd_msan`/`amd_tsan` 6h-hang question above (Task F) is still unanswered — unknown whether it was this same crash looping or an unrelated hang; (b) Gate 3 (live CAS-s3 stateless-lane validation, the lane that originally caught the crash) has not been re-run post-fix — lane validation rides the next CI push of `cas-gc-rebuild`.

- **[fence-window observability] mount-lease keeper is silent at default log level** — GAP (found 2026-07-28, fence-cascade RCA) — During the msan CA-s3 fence window (run for `07f8398acddff2c`) the server log contains ZERO `CasMountLease*` lines for the whole ~7-minute episode: renewal failures, the fence arming, remount start/phases/completion are all invisible; the episode had to be reconstructed from `executeQuery` error timestamps. The keeper's messages exist (the STID-3982 entry above quotes them from an ASan run) but evidently sit below the effective level or fire only on classifier paths. Fix: log at `Information` (rate-limited) — renewal confirm failure with the underlying error, fence armed (with deadline), remount begin, remount recovery milestones, remount complete with duration. Cheap, pure logging, closes gap #3 of `reference_cas_ci_observability_gaps`.
- **[fence-window blast radius] durable writes fail instantly for the whole fence→remount window** — DESIGN QUESTION (2026-07-28 RCA) — During fence→remount (~2 min core window + straggler tails on the msan lane), every durable write returns `668`/`210` immediately; user queries (test INSERTs) get hard errors while an internal `CasWriteRetryLater` lane already exists for system-table flushes. A bounded wait-for-remount on the query write path (block up to N seconds while the self-remount is in flight, then fail) would turn short fence windows into latency instead of failures — the same contract RMT gives during a Keeper reconnect. Behavior change on the write path — needs a design decision, do NOT slip it in as a patch. Related: the remount itself took ~2 min under msan; once the keeper logging (entry above) lands, measure WHERE remount time goes (lease-expiry wait vs recovery replay) before tuning anything.

## 9. Architecture / refactoring (deferred, no behavior change) {#refactoring}

- **[refactor: CasGc split] break `CasGc.cpp` into workflow units** — DESIRABLE — Split scan / reachability / deletion / cursor / budget out of the 2.3k-line file; keep `Gc` as orchestration (pure extraction). Author's second-highest-value refactor. (review1 #13; refactoring-ideas #3.)
- **[refactor: Store de-god-classing] extract remount-thread / caches / ref-append-lane out of `Cas::Store`** — DESIRABLE — 8-responsibility god class; friend-triangle with `Build`/`Gc`. (review1 #13.)
- **[refactor: Store::open modes] split into create / open-rw / open-ro** — MINOR (real bug behind it) — Read-only `Store::open` can still write `_pool_meta` on an empty pool (`PoolMeta::createOrValidate`); make read-only semantics visible (`createOrLoad` vs `loadExisting`) or pass `create_if_missing=false` when `read_only`. (refactoring-ideas #1.)
- **[DiskSelector per-disk isolation]** — HARD / upstream — `DiskSelector::initialize()` has no per-disk try/catch; one unreachable disk aborts disk-selector init server-wide. Pre-existing upstream gap; carve to an upstream PR (Group G). (review1 #5 residual.)
- **[emulated list-token contract] → §14** {#local-backend} (2026-07-23 grooming).
- **[Group G] carve generic Ring-2 fixes into separate upstream PRs** — MINOR (fork hygiene) — Shrinks the fork's long-term conflict surface: `ThreadStatus parent_thread_group` (B90), `ReadBufferFromFileView` (B115), `ReadBufferFromS3` cancel-stop (B117), `LocalObjectStorage` TOCTOU (B38), `MergeTreeDeduplicationLog` null-writer (B37), `copyS3File message_format_string`, `Expect:100-continue` opt-in, `S3Exception::isPreconditionFailed`, GCS conditional dialect + GOOG4 signer, generic conditional-S3-write plumbing. Non-blocking.
- **[weighed refactors — not scheduled]** — see `refactoring-ideas.md` for the WEIGHED-not-mandated set (typed key-wrapper helpers #5, codec-validation/workflow split #6, naming disambiguation #10, backend-test fixture DSL #9). Token-policy (#2) + list-pagination (#7) + delete-outcome classifier (#8) already landed (C1/C2).

## 10. Test coverage & harness {#tests}

- **[GATE-DEBRIS] find the test that writes `test`/`test1`/`test2` into the repo-root cwd** — TEST/INFRA (small hygiene hunt) — `clickhouse-local`'s default database is a filesystem OVERLAY over the cwd, so those debris files shadow `default.test` and deterministically fail ~19 `clickhouse-local` tests in any full run launched from a poisoned checkout (gate-49 cluster A, 2026-07-15; contents = 30/15/10 asterisk bytes, mtime matched the run). Find the producer, make it write under its per-test dir; consider a pre-run debris sweep in the local praktika wrapper.

- **[B200 follow-up] ca-soak scenario card: decommission under load** — TEST — Per spec §testing: a scenario card driving `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` against a live multi-replica pool under workload, plus a chaos variant (kill the command mid-run at each phase, then resume and assert the re-drain completes and the slot retires). Also cover the known fail-closed narrowing: mid-retirement crash on a victim with namespace debris is refused until GC namespace-cleanup catches up (assert refusal, then eventual success).
- **[review #14] highest-risk coverage gaps** — TEST — No `Mode::Native` (real S3/GCS wire) contract-suite row; zero coverage of the `DiskObjectStorageTransaction` CA dispatch/ordering (the B182 fix); concurrency invariants validated only by sequential-logic tests (single real-thread test exists); no tests for `Expect:100-continue`, `S3ObjectStorage::copyObjectConditional` fail-close, `LocalObjectStorage` TOCTOU walk.
- **[pool-hash consistency] CAS blob hash vs `checksums.txt` equivalence test** — TEST — Prove the streaming chunked hash convention yields the same value the part's `checksums.txt` records for each covered file — the dedup/adopt paths silently rely on it; a drift must fail a test, not corrupt dedup.
- **[4h continuous chaos soak]** — TEST/GATE — Blocked by: a compacting object store (rustfs does no background compaction → pool outgrows the pacing cap), streaming/budgeted fsck (B146/B154: summary fsck exceeds its 180s bound at scale → blinds the dangling==0 gate), and a TTL-robust chaos oracle (SOAK-TTL-BAND). Also re-confirms B165.
- **[B146/B154] fsck timeout at scale** — TEST/GATE — Streaming/budgeted fsck for large pools (quadratic discovery/LIST cost at 183 GB / 2.14M objects).
- **[S12 / S22 / S27] infra-gated scenarios** — TEST/INFRA — S12 (10-replica shared pool; N-node `Cluster` was generalized in the 07-06 buildout — wire the compose); S22 (throttling/retry — needs a fault-injecting S3 proxy); S27 (list-pagination ambiguity — needs an instrumented object-store proxy).
- **[ci/full-scale sweep] run dev-scale inconclusives at designed scale** — TEST — RSS attribution, manifest caps, scenario S01–S35 at ci/full.
- **[CA-s3 stateless lane] full-lane run + remaining un-tagging** — TEST (PARTIAL) — Point-fixes landed (04286/05008/05009/01271/03829, B86 removed); run the full lane. Un-tag the remaining `no-content-addressed-storage` tests now that B31 (capability gate) is closed. 3 pre-existing `CaWiring*` GC/shadow gtests fail identically (re-exposed when the sweep filter widened) — root-cause + fix or re-gate.
- **[CI-P1] RustFS provisioning for the CA-s3 functional lane** — INFRA — Add a tracked `setup_rustfs.sh` (mirror `setup_minio.sh`) invoked before the job's `start()`; today it only checks for a pre-existing `ci/tmp/rustfs`.
- **[soak-harness minors]** — INFRA — TTL-band oracle widening for long runs; unreliable pool telemetry at scale (`pool_objects`/`pool_bytes` None); `run_24h.sh` destroys prior-run raw logs at start (move → `logs/prev_<ts>` after a run); S24 needs a pre-agreement `SYSTEM SYNC REPLICA`; S01 scratch high-water sampler misses the OPTIMIZE-FINAL spike; `s3cache` scenario flip to a positive cache-hit assertion; scenario README/cards still say `root_shards` after the S08 oracle rewrite.
- **[RPL-5 slice] `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` queue-clone relink, untested on CA** — TEST — Scoped from the existing RPL-5 finding (`reviews.md`): a `REPLACE_RANGE` log entry from `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` on a Replicated CA table, cloned to a second replica via the replication queue, reduces to a sequence of fetch (relink or byte) + drop — individually working — but there is no integration test proving the CLONED fetch specifically relinks rather than byte-refetches, and RPL-4 documents that `to_detached` relink is explicitly disabled, so it is not obvious a priori which branch a queue-cloned `REPLACE_RANGE` fetch takes. `test_cas_replicated_relink` proves relink for the plain INSERT/merge fetch path only; the freeze/`ATTACH` stateless set (`02271_replace_partition_many_tables`, `01901_test_attach_partition_from`) proves single-node CA correctness for these ops, not cross-replica relink. Deferred out of all-tree Task 12 (2026-07-15): determining the correct relink-eligibility branch for this path is a small investigation, not a copy-paste test — proportionate to do as its own dedicated pass (extend `test_cas_replicated_relink`'s existing 2-replica rustfs fixture with a `REPLACE PARTITION`/`ATTACH ... FROM` scenario + blob-count relink proof), not squeezed into a validation gate. PULLED INTO the 2026-07-23 publish-confirm fetch-handoff iteration's test package (spec `2026-07-23-cas-fetch-handoff-publish-confirm-design.md` §testing) — the iteration touches exactly the relink-eligibility branch this slice needs to prove.

- **[SOAK-FSCK-CHECKPOINT-BUDGET] the soak's checkpoint fsck gate skips itself at scale and the checkpoint still prints OK** — {#soak-fsck-checkpoint-budget} — HARNESS (2026-07-29 T14 soak; recurrence of the `{#gc-observation-vacuous-2026-07-25}` shape at bigger scale) — the entry-gate and post-GC fsck at a GC checkpoint time out at the fixed 180 s budget on a ~29 GiB pool; the harness then SKIPS the `dangling==0` and dryrun-subset asserts (with WARNING lines) yet still prints `GC checkpoint ... OK` with fabricated-looking `reachable=0 dangling=0` — a skip wearing a number. Fix shape (honesty patch, ordered into T14): a checkpoint whose gate did not run must render as GATE-SKIPPED with `not-measured` fields, never OK-with-zeros; separately the budget needs to scale with pool size or the checkpoint needs a cheaper subset probe. Interim evidence path for stage criteria: a post-run `ca-fsck` with an 1800 s budget on the live pool.

## 11. Scalability findings from the full-scale campaign (S3 budget) {#scale-findings}

These are real scale/budget findings; most are variants of "O(N) GC / per-op amplification". Track for the capacity model + a future S3-budget push.

- **[idle-scratch-debris] idle GC leaves scratch files uncollected on an empty pool** — MINOR — S23 (2026-07-18, `2026-07-18-s23-tracked-growth-rca.md` secondary finding): `scratch_bytes` on local staging grew 1→21 MiB over an idle window with ZERO inserts and an empty pool — idle GC rounds appear to create scratch files and not clean them. Local-disk debris, not tracked memory; needs its own check + cleanup path look.
- **[scratch=full-part] CAS write spills the whole object to local scratch for hash-before-upload** — DESIRABLE — 100 GiB merge → 93 GiB scratch; a part larger than local free scratch cannot be written. Largely addressed by the (opt-in) S3-native staging; make the local path stream-hash too, or document the staging requirement for very large parts.
- **[replicated double-spill] shared-pool replica re-merges + re-spills its own full scratch** — DESIRABLE — A replica could adopt the leader's uploaded blob instead of re-merging locally (186 GiB scratch for one deduped 100 GiB blob).
- **[wide-part O(columns)] merge issues O(columns) S3 ops → ephemeral TCP port exhaustion** — DESIRABLE — S07 20000-col `OPTIMIZE FINAL` stalled in an S3 retry storm.
- **[partitioned-INSERT O(partitions)] O(partitions) CAS commits per insert** — DESIRABLE — ~10s per 256-partition insert.
- **[startup O(refs)] server startup S3-op cost scales with #tables/refs** — WATCH — ~152k S3 ops to start a 10k-table server (LISTs/GETs, not blob enumeration); recovery still fast.
- **[S11 capacity] deferred-GC disk accumulation under delete-churn** — WATCH — GC does not reclaim during the delete phase (interval-driven); same O(N)-GC-lag family.
- **[Capacity model] GC cadence + snapshot size under typical load** — DOC/DESIRABLE — Estimate GC frequency + per-shard in-degree run / fold-seal sizes at typical production load; validate against a soak's GC log; feeds the `gc_interval_sec` default and trim gates. Live-AWS data point: a round is 30–40s.
- **[physical-footprint amplification]** — VERIFY — 1h soak: `pool_bytes` ~400× `logical_bytes` (rustfs#3231 overwrite-version retention vs CA debris); should collapse under full GC / a compacting store. Not a safety issue (dangling=0).

## 12. Documentation debt {#doc-debt}

- **[04/05/07 GC-protocol narrative refresh]** — DOC — Prose still describes the pre-refactor world: **three-cursor** merge (now two-cursor), the separate **retired-list run** / `retired_refs` (now `kCondemned` rows + seal `condemned_summary`), and **ack-floor `min_ack` graduation** (now round-paced, writer reads per-hash `.meta`). Concrete stale anchors: `05 §object-key-tree` (`retired/<round>/<shard>`), `05 §magic-table` (`CART`), `07` budget rows (prior-retired-run GET, retired-run PUT). Code + tests are correct; only the narrative lags.
- **[Issue-5 architecture-doc refresh]** — DOC — `01-architecture.md:257` (`/_precommits` shard), `codecs.md` (`root_shards`/`incarnation_*`/`fence_seq` wire fields — all removed), `03-writer-protocol.md` (manifest-backpressure knobs — removed), `04-gc-protocol.md` (`folded_cursor`/lazy-trim), `09-read-protocol.md` (`shardOf`/`root_shards`). Fold in the 04/05/07 refresh above.
- **[cache.md status]** — DOC — Marked "RFC 2026-07-07"; the 5-phase part-folder cache is landed + reviewed. Update to landed state.
- **[03-writer-protocol B172 status]** — DOC — Still lists S3-staging blob upload as TODO; opt-in S3-native staging landed. Update.
- **[codecs.md standardization]** — DOC (proposed) — Complete the magic table (`OwnerProto`/`ServerEpochProto`/`MountLeaseProto`/`FoldSealProto`); decide the `RunRef.checksum` / `PartManifest.payload_digest` CRC-boundary before release; standardize binary version fields (`compatibility_version` vs local `format_version`); type `CARN` record streams at open. `codecs_proposal_v2.md` is the PROPOSED target structure (not adopted; one pre-release cutover).
- **[README refresh]** — DOC — Release-readiness roll-up still lists items now closed (B31) and open (this backlog); refresh once this grooming lands.
- **[07-s3-budget §1.3 all-tree cost row]** — DOC — §1.3 claims mutable-file rewrites cost "0 extra S3 ops (code-derived)"; FALSE post-all-tree for the standalone-rewrite case (a `txn_version.txt` CSN fill-in on a committed part is now a full repoint: stage + precommit + promote PUTs). Still true for the initial whole-part INSERT (carry-forward). Substantive cost-accounting rewrite, not find/replace (T11 review 2026-07-15).
- **[10-backups manifest-sharing rationale]** — DOC — `10-backups.md:287,466` attributes FREEZE manifest-sharing to "mutable per-part fields live in the `RefPayload`"; the sharing *outcome* still holds (same part bytes ⇒ same tree), but the stated *reason* is gone (ordinary content-addressing now, no payload carve-out). Needs a careful rewrite — the dedup claim carries real correctness weight (T11 review 2026-07-15).

## 13. Minor / polish {#minor}

- **[RUSTFS-ERROR-XML] SDK cannot parse RustFS error-body exception name** — MINOR — every RustFS `PreconditionFailed` logs `Unable to parse ExceptionName: PreconditionFailed Message: …` (18.5K in one soak window, `system.blob_storage_log`): the error XML is not AWS-shaped, so the AWS SDK fails to extract the name and CAS recognizes the condition from the message TEXT — works, but brittle against wording changes; add a shape-tolerant parse (or a startup capability note) before relying on it wider.
- **[Issue-6] B3 GC-health columns denormalized onto non-local mount rows** — MINOR — The four `GcHealth` columns are stamped identically on other servers' mount rows (`StorageSystemContentAddressedMounts.cpp:159-162`); NULL them on non-local rows.
- **[F4] CA `MOVE PARTITION` publishes ref CAS before validating the target disk is in the storage policy** — MINOR — S19: 2 `CasRootCas` ops during a correctly-rejected `UNKNOWN_DISK` move (safe, dangling=0); validate the destination disk in-policy before any ref CAS.
- **[snappatch-minor] `CasStore.cpp:2007` replay throw escapes `trySnapshotPublishOnce` without arming publish backoff** — MINOR (defensive) — Dead today by the `min(tail)>newest_snapshot_id` invariant; defensive-pass candidate (likely removed anyway by rev.6).
- **[Build::promote owner-liveness guard is race-only]** — MINOR — Fires only in the narrow promote-vs-dropNamespace window; make the race deterministically testable or remove the guard with a TLA argument.
- **[Ring-2 comment/convention nits]** — MINOR — `S3Common.h` comment overclaims for the RetryStrategy site; `static_assert(DEFAULT_EXPECT_CONTINUE_MIN_BYTES==0)`; `MergeTask::projection_uses_parent_transaction` could be a local; `ProfileEvents.cpp` changelog fragment in a description; `_ms` suffix on a `DateTime64(3)` column; the new `GC REBUILD` right abbreviates "GC" vs the sibling spelled-out "GARBAGE COLLECTION"; internal `cas_part_folder_cache_*` names outlived the key rename; `05011` `no-parallel` tag droppable; empty untracked `poc/` dir husk.
- **[C2-followups] more pagination loops for `forEachListedKey`** — MINOR — Three more identical loops in `CasRefIntake.cpp`/`CasServerRoot.cpp`; `forEachListedKey` also lacks a stop-on-true/page-boundary hook to let `deletePrefixWholesale` + ns-cleanup migrate (interface addition, design first).

## RED (pre-existing, found 2026-07-24): `RefWriterRecoverySeal.EmptyDeadRegionCarveOutStillReportsSameProcessNamespace` {#red-emptydeadregion-phasec}

The Phase C re-mint guard (`6094c1473ea`, "refuse to re-mint writer_epoch 1 while a mount object
exists") makes this seal test unable to open its pool: `Pool::open` -> `mountWritable` ->
`allocateWriterEpoch` (`CasServerRoot.cpp:229`) throws `CORRUPTED_DATA` "no durable epoch object but
a mount lease exists ... refusing to re-mint epoch 1".

Trigger set: {no durable epoch object} x {server-root data subtree EMPTY} x {mount object Present} x
{normal mint policy}. Only this test constructs it — `seedUncleanPredecessorMount`
(`gtest_cas_ref_writer.cpp:2805`) writes a mount through the production `claimMount` and nothing
else, and this is the only seal test whose dead region is deliberately empty; all 11 siblings pass.

Assessment (not a fix): in production that state looks UNREACHABLE, because `mountWritable` calls
`allocateWriterEpoch` — which writes the epoch object — BEFORE `claimMount` publishes the mount. On
that reading the guard is right and the FIXTURE is unrealistic (a real predecessor at epoch 1 leaves
an epoch object behind), so the fix is to seed the epoch object for this one test rather than to
weaken the guard. NOT applied: the guard and the test are the same parallel round's just-landed work,
and if the state turns out to be reachable by some other order, refusing it would brick a pool — that
is a product question for the guard's author, not a test tweak to guess at.

Why it survived its own landing commit: the narrow `Cas*:CA*` gate filter EXCLUDES `RefWriter*` (the
2026-07-17 gate-filter gap). Use the comprehensive filter.

## [GC-THROUGHPUT-COLLAPSE] dropped namespaces are never pruned once GC rounds stall {#gc-throughput-collapse}

Initial 2026-07-24 finding (63% of a CI pool's listed volume sat in already-dropped namespaces once
GC rounds stalled under churn); superseded by the fuller 2026-07-25 RCA at
[`{#gc-throughput-collapse-2026-07-25}`](#gc-throughput-collapse-2026-07-25), which identifies the
same mechanism (rounds are all-or-nothing — no cleanup runs until the fold reaches its commit point,
and a big-enough universe makes that point unreachable within the round interval) with the three
separable defects tracked there. Not the same class as `[codex-11]` (an empty ownerless ref-table
revived through a TOCTOU) — this is cleanup throughput, not a correctness defect.

## [04286 timeout] `system.remote_data_paths` walks EVERY disk — 600s timeout on the CA-s3 lane {#remote-data-paths-no-pushdown}

Root-caused 2026-07-24 from the PR-2073 CI logs. `04286_content_addressed_remote_data_paths` timed
out at 600s; the server finished the query only at 1181s. NOT a CAS regression and NOT the
mount-lease path (zero `mountWritable`/`claimOwner`/fence lines in the window — the fence-not-rescue
round is exonerated). The test is unchanged since `84d93c2f817` (2026-06-29) and PASSES on the
CA-local lane in the same run.

Cause is generic ClickHouse, not CAS: `StorageSystemRemoteDataPaths` walks every disk on the server
because `WHERE disk_name = …` is not pushed down — the TODO is still in the source
(`src/Storages/System/StorageSystemRemoteDataPaths.cpp:153`, "void applyFilters(...) can be
implemented to filter out disk names"). On this lane that walk covered two PUBLIC-INTERNET web disks
(168 probes, ~19s) and then a recursive paginated LIST of the shared CA-s3 pool's ref namespace for
all ~11k parallel tests: 872 LIST requests at ~1.3s each, of which 436 (50%) timed out on the first
attempt and succeeded on retry. No wedged lane, no stuck GC round, no lease failure.

So it is scale/latency dependent and will RECUR non-deterministically on the CA-s3 lane whenever the
shared pool is large. FIX (highest value): implement the `applyFilters` disk-name pushdown, which
makes this test cheap and removes a general foot-gun for anyone querying `system.remote_data_paths` on
a server with slow/remote disks. **That file is generic upstream code, so per the standing rule it
needs consultation before editing** — flagged rather than patched. Without the pushdown, the test
cannot be made safe on a lane whose default disk is a shared CA-s3 pool.

Mitigation landed 2026-07-27: the test is tagged `no-content-addressed-storage` (its coverage is the
inline CA disk it creates itself, so the CA-default lanes added nothing but the pathological walk —
it recurred in the 2026-07-26 run at 600s + a "Some queries hung" ride-along on the asan lane). The
pushdown item above stays open as the real fix.

## [CA-s3 Disk session pressure] `ConnectionGroup: Too many active sessions in group Disk` (noted 2026-07-27) {#ca-s3-disk-session-pressure}

On the asan CA-s3 lane (run for `e2d04bfe37e`), `00149_quantiles_timing_distributed` flipped on a
leaked stderr warning: `ConnectionGroup: Too many active sessions in group Disk, count 10400,
warning limit 8000`. The test's stdout was correct and reruns passed — the failure is warning noise,
but 10k+ concurrently active Disk-group sessions under parallel load is a real pressure signal for
the CA-s3 request fan-out (compare the write-path request-class findings in
`{#disk-error-audit-followups-2026-07-21}` and the insert-slowness item). Worth a look at whether CA
holds S3 sessions longer than needed (e.g. across retry backoffs) before raising any limit.

## 14. Local / emulated backend {#local-backend}

Collected 2026-07-23 (user direction): every "local backend" story lives HERE, so the class is visible
as one body of work instead of scattered minors. Common root: `LocalObjectStorage` writes are plain
`O_TRUNC` file writes — no atomic PUT, no conditional-write enforcement, no torn-read protection —
while the CAS protocol is designed against S3 atomic/conditional semantics. Nothing in this section
affects S3/GCS production pools.

- **[disk-error-audit] temp-file + rename in the local blob write path** — HARD — (moved from the
  2026-07-21 disk-error audit) `emuWrite` (`CasObjectStorageBackend.cpp:546-557`) streams through
  `LocalObjectStorage::writeObject`, which opens a plain `WriteBufferFromFile` directly on the final
  key with `O_TRUNC` (`LocalObjectStorage.cpp:250-277`). ENOSPC or a kill mid-write leaves a partial
  file at the final content-addressed key; the next `putIfAbsent` sees `emuExists == true` and returns
  `PreconditionFailed` = "already present" (`:776-780`), so the writer dedups against the truncated
  body. Presence-only admission + non-atomic local write is the ONLY corruption window the audit
  found. Native/S3 mode is not affected (`If-None-Match` + atomic completion). Fix: write to a sibling
  temp name and `rename` into place (or fix it inside `LocalObjectStorage`), paired with the
  dedup-admit size guard (still in the audit section) as defense-in-depth. Fixing this also closes
  B66a's torn-read mechanism below.
- **[B26 / B135] local / NFS / shared-fs as a first-class backend** — DESIRABLE — Unit-tested over
  `LocalObjectStorage`; needs server-level doc + the put-if-absent atomicity caveat (racy multi-writer
  on local/NFS) + multi-mount safety notes. (B66a is the concrete instance of the caveat.)
- **[B66a] concurrent-fetch torn read of a shared `detached` ref on local storage** — MINOR —
  `LocalObjectStorage` write is not atomic; a concurrent reader/writer of the SAME ref key can observe
  a half-written object. Safe on S3 (atomic PUT). Freeze dodged this class by design (one ref per
  frozen part, no shared container — `CONSOLIDATION-COVERAGE.md`); the residual case is concurrent
  writers of one `detached/<part>` name. Mechanism is closed by the temp-file+rename item above;
  until then racy multi-writer on local/NFS stays documented-unsafe. Deliberately OUT of the
  2026-07-23 reserved-precommit iteration (orthogonal to the handoff protocol).
- **[emulated list-token contract]** — MINOR / VERIFY — (moved from §9) `ObjectStorageBackend::list`
  in `EmulatedSingleProcess` mode may still return a different token kind than
  `head`/`get`/`put`/`delete` (child etag vs `emuObserveToken`), a Liskov gap vs `supportsListTokens`.
  The token-policy centralization (C1, landed) added `tokenForHead/tokenForList/tokenMatches`; verify
  the emulated `list` path now agrees or make `supportsListTokens()==false`.
- **[STATELESS-04286 EISDIR]** — pointer — `existsFile` mountpoint probe throws "Is a directory"
  (EISDIR) on the LOCAL CA backend; tracked in `utils/ca-soak/scenarios/BACKLOG.md`
  (STATELESS-04286-getmountpoint-eisdir, 2026-07-08); needs a fail-closed-semantics decision +
  re-check on RustFS/S3.
- Related, landed: `LocalObjectStorage` TOCTOU walk fix (B38) — upstream carve-out tracked in §9
  Group G.

## 15. Found during the 2026-08 documentation consolidation {#consolidation-2026-08-findings}

New items surfaced while writing/verifying the docs-consolidation pages (2026-08-01 to 2026-08-04);
continuing the ID series, not renumbering anything above.

- **[gate-filter-countingbackendshape-escape] gtest suite `CountingBackendShape` escapes the `CAS*` filter** — TEST/INFRA — Found during Task 14 (AGENTS.md) review: this suite does not carry a `Cas`/`CAS` prefix, so it is invisible to the `CAS*` gtest gate filter documented as the covering mechanism. Same class as the three prior gate-filter-gap findings in this file (§10, §list at `#gate-filter-gap-3-backend-contract`) — rename the suite or extend the filter.
- **[gc-anomaly-never-emitted] `CasEventType::GcAnomaly` is defined but never emitted** — MINOR — Found during the deep-verification batch (batch-006): the event type exists in the enum but no call site constructs one, so any doc or dashboard describing GC-anomaly events as observable is currently wrong. Either wire an emit site or remove the dead enum value.
- **[reftxnid-wraparound-guard-missing] `nextRefTxnId` lacks a `UINT64_MAX` wraparound guard** — MINOR — Found during the deep-verification batch (batch-021, cluster C-0514): the sibling counter at `CasRefProtocol.cpp:941` has an explicit wraparound guard; `nextRefTxnId` does not. Add the matching guard or document why it is provably unreachable.
- **[system-md-missing-cas-verbs] `SYSTEM CAS` verbs missing from `docs/en/sql-reference/statements/system.md`** — DOC — Found during Task 12 (operations runbooks): `SYSTEM CAS FSCK`/`FORGET`/`GC STOP`/`GC START` (and siblings) are documented in the CAS-specific pages but absent from the generic `SYSTEM` statement reference, where a user would naturally look first.
- **[casrequestcontrol-comment-settings-stale] `CasRequestControl` header comments cite settings that do not exist** — DOC — Found during the Task 12 fix round: the header comments name `cas_s3_retry_initial_backoff_ms`/`cas_s3_retry_max_backoff_ms` as if they were configurable settings; they exist only in the comment text — the real budget is hardcoded in `CasRequestBudget`. Either implement the settings or fix the comments to stop implying a configuration surface that isn't there.
- **[s3cache-config-comment-stale] stale comment in `utils/ca-soak/configs/storage_conf_s3cache_ch1.xml`** — MINOR — The comment claims cache-over-CA fails with `NOT_IMPLEMENTED`; this was fixed by `3ed0e5f5030` (2026-07-08) and the cache-over-CA path is now live-validated (see the quick-start cache example, `380688e8a66`). Remove the stale comment.
- **[part-folder-validate-never-gating] `part_folder_validate=never` needs a gate, not a silent accept** — HARD (user settings-policy direction) — `PartFolderAccess.h:135-138` accepts `never` (skip the `ForceFresh` body re-proof entirely) with no acknowledgment of the risk. Either remove the `never` value or require an explicit risk-acknowledgment setting alongside it. Docs already carry a strong warning on this value (`configuration.md`); the code should not make it this easy to select silently.
- **[gc-enabled-false-silent] `gc_enabled=false` accumulates garbage silently** — HARD (user settings-policy direction) — Disabling the background GC scheduler produces no ongoing signal that reclamation has stopped. Add a periodic warning log line plus a metric while `gc_enabled=false` and the pool has reclaimable debris, so an operator who disabled GC for a legitimate reason (or by mistake) finds out before the pool grows unbounded.
- **[dedup-presence-only-window-recheck] re-verify the deduplication presence-only-admit corruption window** — VERIFY (user-flagged from memory, re-derive from the disk-error audit) — The 2026-07-21 disk-error audit (`#disk-error-audit-followups-2026-07-21`) identified a presence-only dedup admit as a corruption-window class; re-verify against HEAD whether this window is still open post the format/staging changes since that audit, and either close it out or fold it back into an active item with current evidence.

## Recently closed (2026-07-13 grooming — do NOT re-open) {#recently-closed}

Verified DONE at HEAD; recorded so they are not re-triaged:

- **[refactor: DiskObjectStorageTransaction part-path virtualization]** — DONE-by-deletion (all-tree
  part-files Task 6, `430216ad1ef`): the item's target — the CA eager-dispatch rename hook (B182,
  `isContentAddressedMutablePartFileRename`) — is deleted outright rather than abstracted behind a
  virtual; its only trigger (`txn_version.txt.tmp` → `txn_version.txt` MVCC rename) no longer exists
  on a CA disk since Task 5's `supportsAtomicFileWrites` short-circuit writes `txn_version.txt`
  directly (see `DiskObjectStorageTransaction.cpp::moveFile`). Also closes the mutable-set removal
  more broadly: `uuid.txt`/`txn_version.txt`/`metadata_version.txt` are now ordinary manifest entries
  (Task 6), and a committed-file unlink stages a removal mark resolved via repoint (Task 8) — the
  `mutable_files`/`mutable_removed`/`isMutablePerPartFile` fields/predicate themselves are legacy,
  pending Task 9's schema-deletion sweep. (review1 #13.)
- **B200 pool-member decommission** — DONE 2026-07-15 (spec `2026-07-13-cas-pool-member-decommission-design.md`, plan `2026-07-13-cas-pool-member-decommission.md`): `Store::openForDecommission` admin claim gate `03b3b95de44`; `decommissionPoolMember` core `e0e83e8521d` + precommit-count fix `1b5a7f5faf3`; drain sweeps `eb8f78adef2`+`5780b6ec646` + manifest-debris tolerate-and-continue fix `3d641996a1a`; slot retirement `6c86deebd1a`; SQL surface `70599d30cf4`+`0def36c2f7e`+`51ff6879864` (ON CLUSTER round-trip fix); disks facade `ca-drop-member` `e375fafa5e0`. Integration test = Task 7 (landing separately). Follow-up scenario card → §10.

- **Whole stabilization iteration** (`docs/superpowers/plans/2026-07-12-cas-stabilization-cleanup.md`) — every task committed (A1 `~Store()` teardown abort, A2 RunFile OOB, A3 flushRefBatch wedge, A4 EDGE-BEFORE-OBSERVE throw, A5 Ordinary-detached namespace, A6 skip_access_check, A7 stable-Gc manual round, A8 fold-seal enum range-check, A9 dropRefBestEffort logging, A10 suppress_destructive once; B1 path memoization, B2 explain-journal opt-in, B3 per-disk GC health, B4 late-predecessor counter [→ to be removed by rev.6]; C1 token policy, C2 forEachListedKey + delete classifier, C3 blobKey in CasLayout.cpp, C4 unified dir dispatch, C5 whole-part-txn encapsulation; D1–D5 dead-code/vestigial removal; E1 GC-REBUILD access right split, E2 config naming, E3 typed mounts columns; F1 delete `poc/cas_mergetree`; R412 one 412 policy, RExpect scoped 100-continue).
- **Umbrella review `review1.md`** — findings 1, 3–12 + minors fixed by the stabilization iteration; finding 2 (relink "RBAC") retracted as not-a-bug and documented (interserver channel == ordinary `ReplicatedMergeTree` trust boundary); only findings 13 (god-class/virtualization refactors) and 14 (coverage gaps) + `DiskSelector` isolation carried forward (§9, §10).
- **B31** capability gate — `supportZeroCopyReplication()==false` for CA with a B31 comment; unsupported ops rejected by independent gates (ALTER PARTITION throws, BACKUP restore routes through a whole-part transaction). **B192** event-name review — 51 event types all neutral snake_case, no flagged terms. **B10 minors** `~Build`/`retireBuildSeq` (no I/O on the dtor path), `inDegreeInGeneration` (test/preview-only), redundant `sweepNamespace` watermark GET, signed-in-degree accumulation (unsigned edge-set model) — all fixed. `RunFileReader::seek` FIRST-block-≥-target contract bug fixed (`035edbcf7e1`). Vestigial manifest-backpressure surface removed (`e743da297bb`). B207 fsck phantom-dangling, B3/B186 `FreezeViaHardLinks` red, deposed-leader `clearSparedMeta` — all RESOLVED (2026-07-11).

## Obsolete / superseded (removed or to be removed) {#obsolete}

- **[B1] `manifest_hash` on the Keeper `/parts` znode — REJECTED (2026-07-14), do NOT re-open as a Keeper field.** Layering decision: replication code stays disk-agnostic — a CA-specific field in the Keeper part header (`ReplicatedMergeTreePartHeader`, `commitPart`/`getCommitPartOps`) would couple `ReplicatedMergeTree` to one disk implementation, grow the upstream-contact surface of the fork, and add states (plus znode-format/mixed-version evolution) to already-complex replication machinery. It is also unnecessary: fetch-by-relink learns the manifest id **in-band** (interserver handshake, `DataPartsExchange`); real data divergence is already caught by the stock tolerant `checkPartChecksumsAndCommit` → `checkEqual`; and manifest-level divergence between replicas is a benign, bounded, self-cleaning dedup-MISS (`01-architecture.md §benign-cross-replica-divergence`). If divergence *observability* is ever wanted, it is a pool-side concern — fsck/`ca-inspect` can group live refs by (table, part name) across server namespaces and flag differing manifest ids — never a Keeper field.
- **Root-shard-axis items are MOOT** — the mutable `RootShardManifest` / per-`(ns,shard)` root-shard ref model was **replaced by the per-table snapshot+log** (`PoolMeta.root_shards` deleted, `Store::shardOf` gone). The following ROADMAP items no longer apply: **per-namespace `root_shards`** (DESIRABLE), **adaptive shard SPLITS** (DESIRABLE), **root-shard fan-out vs per-object permit cap (B158)**, and the **shard-mutation flat-combining queue** (`specs/2026-07-03-cas-shard-mutation-queue.md`; superseded by the `CasSingleWriterSlot` per-table ref-append lane — its `CasShard*` ProfileEvents were removed in stabilization D2). **B111** (`RENAME` = one Build/part, "multiple root-shard updates per rename") is likely moot/reframed under the per-table ref log — revisit only if a rename cost shows up.
- **`CasRefLatePredecessorObserved` (B4)** — landed (`10274550bb3`), to be removed with rev.6 (grace-window machinery goes away). See §1.
- **`deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`** — SUPERSEDED+IMPLEMENTED (A1 ranged reads + A3 streaming `readPriorEdges` landed via T2/T0); only the T1 delta-runs residue remains, tracked in §2. File removed in this grooming.
- **Superseded specs/plans (kept for history, banner-marked):** `specs/2026-07-10-cas-ref-snapshot-log-design.md` (GC-owned-base model → superseded by the 2026-07-11 writer-owned rev.5); `plans/2026-07-10-cas-meta-descriptor-raw-body.md` (REJECTED — recreated the rejected generation-in-key model; Phase B landed as freshness-v3 instead); `plans/2026-07-09-cas-promote-resurrect-tokened-blob.md` (landed then removed by writer-GC-simplification Phase A / EDGE-BEFORE-OBSERVE); `specs/2026-07-01-cas-shard-incarnation-and-registry-removal-*` (registry removal DONE via D1; RootShard incarnation moot under snapshot+log).
- **P3.1 lease-view-sync-decouple Task 5** — likely MOOT: freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`, so "decouple renewal from the retired-view beat" has no beat to decouple from. Confirm + close (§8).

## TXN-ONE-PIPELINE follow-up: committed-ref DDL overlay (deferred 2026-07-16)

The TXN-ONE-PIPELINE initial landing (plan `docs/superpowers/plans/2026-07-16-cas-txn-one-pipeline.md`, decision Tension 1) implements the one-overlay invariant for the **part-build write path only**. Committed-ref DDL ops (DROP/MOVE/RENAME TABLE via `removeDirectory`/`removeRecursive`/`republishRef`/`dropNamespace`) and verbatim table/mountpoint files stay the **immediate class** (durable-at-call-time), not overlay-deferred.

Follow-up = execute Appendix-A Tasks 1.2 / 1.3-DDL / 1.4-DDL (a `pending_ref_ops` overlay `{Drop,Move,Replace}` keyed by `(ns,ref)`, materialized in `commit` after `publishStaging`, plus the new read surface so transaction reads answer "ref dropped/moved"). Deferred because: (a) fixes NO known motivating bug (01603/B58/B63 are all part-build); (b) highest regression risk — interacts with the empty-cover `commitTransaction` workaround (Audit 7, KEPT) and DROP/DETACH/ATTACH rollback. Gate before doing it: audit that no single CA transaction interleaves an overlay-deferred part op with an immediate DDL/verbatim op order-sensitively (if it does, that case must be handled). Tasks are fully written in Appendix A, ready to execute.

**Abandon-path note (added 2026-07-16, Phase 3):** the funnel converts durable DDL/verbatim ref-ops from commit-time-queue-drain to CALL-TIME (immediate class). Like `moveDirectory`/`createHardLink` already do, they then apply at call-time and are NOT compensated on abort — consistent with spec §Transaction-Model ("abort does not compensate an early destination ref"). The clean-abort behavior for DDL ops (overlay-discarded on abort) is precisely what this deferred DDL-overlay follow-up delivers. Interim risk = a DDL/ALTER that applies a durable ref-op then aborts before disk commit leaves the early-applied drop; narrow, not hit by the INSERT-sink abort, and covered by the DDL-exercising stateless/soak gates (a dangling/lost-part there = STOP + pull that op's overlay-deferral forward).

## CA write-path allocation / memory audit (noted 2026-07-16, from TXN-Final stateless trace_log)

During the TXN-Final full CA-default stateless run, `system.trace_log` showed the CA write path dominates the Memory (allocation-sampling) trace: `ContentAddressedTransaction::tryCreateWriteBuffer` (~489k samples) + `writeFile` (~488k), then `CaInlineWriteBuffer` (~322k) and `CaContentWriteBuffer` (~165k). CPU was clean (NO CAS symbol in the top-15 CPU stacks; GC/writer background loops appear only in Real = idle, no busy-spin) — so this is NOT a CPU or correctness issue, purely an allocation-volume observation.

TODO — a deliberate alloc-profile pass on the CA write path (use `.claude/tools/alloc-profile` / the alloc-profile skill):
- Is `tryCreateWriteBuffer` (the TXN Phase-3 write-buffer hook) allocating more than necessary per file? It builds a write buffer + a `std::function` finalize closure + captures an `owner shared_ptr<IDiskTransaction>` per file — check for avoidable per-write allocations / whether the closure+shared_ptr can be slimmed.
- inline vs content buffer split (322k vs 165k) is expected (more small inline files), but confirm `CaInlineWriteBuffer` isn't growing its buffer inefficiently.
- Minor: `Cas::ObjectStorageBackend::emuWrite` / `putIfAbsent` take `std::map<std::string,std::string>` headers BY VALUE (copies) — pass by const-ref. (Emulated `local_blob_storage` test path; real S3 backend differs, but the copy is gratuitous.)
- Establish a baseline: compare CA-part-write allocation profile pre- vs post-TXN-ONE-PIPELINE to confirm the write-hook refactor didn't add per-write allocation overhead.
Not correctness-blocking; a perf/memory-efficiency item.

## Non-CA stateless fast-fails to re-check on a clean CI box (noted 2026-07-16, TXN-Final)

The TXN-Final full CA-default stateless (run on a CONTENDED DESKTOP workstation — load 10, a chown at 88% CPU, zoom) produced 38 fails, of which 32 were resource-contention TIMEOUTS (300-600s on trivial tests; 00050_min_max literally "Timeout! Killing process group" @600s) — NOT correctness failures. The remaining fast-fails are NOT attributable to TXN/codecs (all changes are in Disks/.../ContentAddressed + checkAlterPartitionIsPossible removal + ManifestEntry::size()):
- INFRA (ignore): 02479_mysql_connect_to_self (mysql server down), 02784_connection_string (::1:9000 refused).
- Result-diffs in unrelated subsystems: 01854_s2_cap_union, 02224_s2_test_const_columns (S2 geo funcs), 03233_dynamic_in_functions (Dynamic type), 00163_shard_join_with_empty_table (distributed join). A storage-layer change cannot alter these results → pre-existing branch state or randomized-settings flakes.
TODO: re-run the CA-default stateless on a QUIET box (or real CI) to (a) confirm the timeouts vanish without contention, (b) confirm the s2/dynamic/shard-join diffs are pre-existing/upstream (not this campaign). Related: the CA write-path alloc note above (if CA writes are slow, they compound the contention timeouts).

## CA ref-table copy on the commit/ref-op path (noted 2026-07-16, TXN-Final soak trace_log)

The #1 CPU stack in the TXN-Final soak (pure-CA workload) was `std::__tree<...DB::Cas::RefCommittedRow>::__copy_construct_tree` (272 of ~272 CPU samples) + `__tree_deleter` (174) — i.e. deep copy-construct + destroy of the whole committed-ref map `std::map<std::string, DB::Cas::RefCommittedRow>` (RefTableState). Call path: `Cas::Store::appendRefOps` / `flushRefBatch` → `ContentAddressedTransaction::commit` → `publishStaging`. (Overall CPU is LOW — 272 samples/15min, CA is I/O-bound — so not a current CPU hog, but a SCALABILITY smell.)

Concern: every ref op on the commit path appears to copy the entire ref-table state by value (the `appendRefOps` `std::function<vector<RefOp>(RefTableState const&)>` snapshot). Cost grows with ref-table size → ~O(refs) per commit, ~O(refs·commits) over a workload — compounds under insert/mutation-heavy loads and likely contributes to CA-storage slowness (see the stateless timeout note + the write-path alloc note — all commit-path overhead).

TODO (perf, likely pre-existing ref machinery, NOT a TXN regression — TXN moved publish TIMING, not this copy): investigate whether the RefTableState snapshot in `appendRefOps`/`flushRefBatch` can be passed by const-ref / diffed incrementally / copy-on-write instead of full-copied per ref batch. Confirm pre-existing via git blame on appendRefOps/flushRefBatch. Alloc-profile the commit path together with the write-buffer note. Not correctness-blocking (soak green, dangling=0).

## Source-layout mechanical-phase intermediate commits not clean-buildable (noted 2026-07-16)

During Step-4 source-layout, the Phase-2 include sweep fixed includes in 3 EXTERNAL consumers (DataPartsExchange.cpp, DiskObjectStorageTransaction.cpp, MetadataStorageFactory.cpp — all OUTSIDE the CA dir) but the sweep commit's pathspec (CA-dir + CMake only) STRANDED those fixes uncommitted. So commits 592b9b8..9d714dd8 reference moved CA headers at dead paths → NOT clean-buildable from a fresh checkout (per-step gtest "green" passed only because incremental builds saw the uncommitted working-tree fixes). Caught by controller verifying the COMMITTED state (git show HEAD:file) vs the moved-header existence, not the working tree. Fix = a follow-up commit committing the 3 stranded external include fixes (makes HEAD-forward clean). The intermediate commits stay non-clean-buildable (no-amend/no-rebase rule) — a BISECT HAZARD on this dev branch: a bisect landing in 592b9b8..9d714dd8 fails to build the external consumers. Acceptable (dev branch, not upstream), documented here. LESSON: (a) reorg commit pathspecs must include ALL sweep-touched files incl. external consumers; (b) verify the COMMITTED state builds (git stash + build, or fresh checkout), never trust incremental-build green for a move/sweep.

## Source-layout post-decomposition CasStore follow-up candidates (noted 2026-07-16, Task 3.6 checkpoint)

Task 3.6 composition-root checkpoint on `Pool/CasStore.{h,cpp}` (HEAD `4278e999e40`, after 3.1–3.5): `CasStore.cpp` = 1184 lines, `CasStore.h` = 771. The spec §CasPool-After "~400" target is a STALE pre-3.5 estimate — it did not budget the mount claim/recovery ORCHESTRATION that 3.5 correctly kept inline (`open`+`mountWritable`+`openForDecommission`+`tryRemountOnce` ≈ 471 lines alone, legitimately on the pool). All PLANNED components (CasPlainObjects/CasManifestReader/PoolConfig/CasRefLedger/CasMountRuntime) are extracted; the enumerated inline residents are all correct. Honest post-decomposition size ≈ 1180 is accepted (mostly irreducible mount protocol). Two stray inline blocks flagged as OPTIONAL future component candidates (NOT extracted now — not in the spec's component set, and no drive-by scope-creep during the mechanical refactor):
- **LIST-discovery** — `listNamespaces` (~67) + `listMirroredChildren` (~45) ≈ 112 lines: stateless LIST-based namespace/child discovery over backend+layout, same shape as `CasPlainObjects`. A clean stateless-service extraction candidate; would take CasStore.cpp toward ~1070.
- **Anomaly-policy** — `reportImpossibleInterference` + the `peekForeignRefLogHeader` anon-ns helper (~113 lines): deliberately kept on Store by the 3.5 mount plan (it drives `tripMountLost`/`scheduleRemount` + a detached diagnostic thread that stays on Store). Could co-locate with `CasMountRuntime` in a later pass; was an intentional 3.5 decision, not an oversight.
Extracting both would reach ≈ 960 — still far from 400 because the mount protocol dominates. Low priority; the composition root is sound as-is.

## Phase 4 (CasBlobUploader) DESCOPED — writer-protocol blob lane, not a separable byte engine (decided 2026-07-16)

Step-4 source-layout Phase 4 ("extract Pool/CasBlobUploader from CasBuild") is DESCOPED by decision (user-confirmed), backed by TWO independent read-only reviews (srclayout3 + a fresh Fable 5 consult) that agree. NOT done in any form (no CasBlobUploader.{h,cpp}).

Finding: the spec's clean "byte-delivery engine vs transactional decision" split is NOT realizable — it's a control-flow fiction against the actual code:
- `putBlob` → `observeAndAdmit` → (throws ABORTED) → `uploadFromSource` → `observeAndAdmit` is ONE mutually-recursive machine (putBlob's bounded retry loop re-enters on the ABORTED thrown mid-flight). `observeAndAdmit` *is* the adopt/resurrect CHOICE (condemned point-read) FUSED with admit execution — the decision materializes mid-delivery because write-once conditional PUTs use the 412 as the discovery mechanism (decide-while-delivering is the protocol design, not a factoring accident).
- A "pure I/O engine returning an outcome struct" fails: (a) the `precommitted` EDGE-BEFORE-OBSERVE guard must fire BETWEEN the condemned meta point-read and the adopt side effects (meta-backfill PUT + BlobReuseAdopt event) — moving the decision out means the engine already did a backend-visible write on the violating path = behavior change; (b) `lm_before` (LoadedMeta etag captured before the decision) is reused as the CAS precondition mid-flight — decision + execution share a datum; (c) W-FRESH-TAG: buildHeader mints a fresh incarnation_tag on EVERY attempt from build_id/info, so build identity can't cross as data, only as a factory. An outcome protocol = a control-flow REWRITE (driver-loop + coroutine), the opposite of a trustworthy relocation.
- No locks move (only atomic `cancelled`, decision state, stays). Real env would converge on `Store&` + build identity + Build mutable state = Build itself.
- Decisive argument: these methods ARE the blob lane of the WRITER PROTOCOL, not a byte engine that happens to live in Build. EDGE-BEFORE-OBSERVE's guard licenses `promote`'s no-probe trust; `deps` (W-DEP-SET) wires them together. Splitting one invariant's text across two files joined by ~8 std::function callbacks makes the audit HARDER — coupling laundered through function pointers is still coupling, minus its names. observeAndAdmit/uploadFromSource are private, single-consumer (only Build::putBlob), single-threaded, transaction-scoped.

Optional future work (NOT scheduled): (1) if file length itself is ever the complaint, promote the genuinely Build-state-free lambdas to free functions verbatim — `buildHeader`→`makeBlobEnvelopeHeader`, `writeFreshMetaClean`, `writeResurrectMetaClean` (pure fns of args) — shortens uploadFromSource ~300→~200, moves ZERO decisions (cosmetic). (2) A real separable design would require extracting the adopt/resurrect DECISION as a pure "plan" first, THEN a stateless executor becomes possible — a genuine redesign, not a mechanical move. Neither is worth it now; CasBuild stays whole.

## Source-layout Phase-5.2: *Build* helper-method naming consistency (LOW-PRI, noted 2026-07-16)

After the `Build`→`PartWriteTxn` class rename (`e3a165dfa15`), a handful of helper/method names still contain "Build" as an English/protocol word and were deliberately NOT renamed (the spec's Task 5.2 map was narrow: only `Cas::Build`/`BuildPtr`/`BuildInfo`/`startBuild`→`beginPartWrite`): `promoteBuild`, `registerInflightBuild`, `cancelInflightBuildsForNamespace`, `startBuildFor`, `precommittedBuildFor`, `startStagingBuild`. Several are arguably CORRECT as-is because they operate on the protocol `inflight_builds` registry / the protocol "build" lifecycle concept (which the spec DELIBERATELY spares — build_seq/buildSeq/BuildPrefix/inflight_builds vocab unchanged). Residual reads slightly mixed (e.g. `PartWriteTxnPtr registerInflightBuild(...)`). LOW-PRIORITY optional follow-up: decide per-method whether "Build" there means the (renamed) class or the (spared) protocol concept, and rename only the former. Not worth expanding the rename diff now; no correctness impact.

## Full CA-default stateless: run on real CI / quiet box (confirmed again 2026-07-16 post-source-layout)

Post-source-layout CA-default stateless (HEAD e3a165) = 0 CA regressions (every content_addressed test passes), but 40 non-CA fails: dominated by CONTENTION TIMEOUTS (15 tpc_ds queries @600s ceiling, distributed/uniq/join/parallel_hash/avro @300-600s; 68 timeout markers) + pre-existing non-CA diffs/infra (s2 01854/02224 @0.2s, dynamic 03233, mysql 02479, ipv6 01880, connection_string 02784). This is the SAME workstation-contention + pre-existing profile as the TXN-final run — the 553×N parallel suite self-contends on this single workstation, so the full suite's non-CA pass-rate is not trustworthy here. TODO: run the full CA-default stateless on real CI (or a genuinely quiet box) to get a clean non-CA baseline; the CA-subsystem signal is already clean locally. NOT a refactor regression.

## PRODUCT BUG (found by S36, 2026-07-16): MOVE PART TO DISK 'ca' fails — promote unique-ref violation on ref 'moving'

Scenario S36 (MOVE PART/PARTITION between local and CA disks), HEAD e3a165, multidisk cluster: after a clean bring-up + 2/2 setup verdicts, the core leg
`ALTER TABLE s36_move MOVE PART '0_0_0_0' TO DISK 'ca'`
throws HTTP 500 Code 236:
`DB::Exception: promote: ref 'moving' already names a different committed manifest — refusing to overwrite (unique-ref invariant; use republishRef for an intended repoint). (ABORTED)`
i.e. the MOVE-PART-to-CA path publishes the moved part's manifest through the CAS `promote` path (fresh publish, enforces the unique-ref invariant) under a ref named `'moving'`, which collides with an existing committed manifest named `'moving'`. The error message self-hints the intended mechanism (`republishRef` = intended repoint) vs `promote` (fresh unique publish).

STATUS: ROOT-CAUSED (2026-07-16, scen3637b), two confirmed layers, NOT a quick fix — escalated to controller rather than fixed solo (large/architectural, per protocol).

**Layer 1 — path-parser gap (mechanical, well-precedented pattern, but not the whole story):**
`PartPathParser.cpp:findPartDirComponent` (Atomic-uuid branch, ~line 150-164) special-cases `kDeduplicationLogsDirName` (returns `nullopt`) and, separately, `ContentAddressedMetadataStorage::route()` special-cases `kDetachedDirName` (`ContentAddressedMetadataStorage.cpp:646-659`, re-splitting `p.file`'s first component as the real part and prefixing the ref with `detached/`) — but there is **no equivalent case for `MergeTreeData::MOVING_DIR_NAME` ("moving")** (`MergeTreeData.h:221`) anywhere under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (confirmed via `grep -rn "moving"` on that whole subtree: zero hits). A part being moved is first cloned under the MergeTree-standard staging path `<table>/moving/<part_name>/<file>` (`MergeTreePartsMover::clonePart`, `MergeTreePartsMover.cpp:281`, `part->makeCloneOnDisk(disk, MergeTreeData::MOVING_DIR_NAME, ...)`). Because `route()` has no `moving` branch, EVERY file of EVERY part ever moved onto a `content_addressed` disk resolves to the exact same literal ref name `"moving"` (the parser's generic uuid-anchor rule treats the component right after `<uuid>` as `part_name` verbatim — which for `tmp_merge_*`/`delete_tmp_*` etc. is fine since those directory names are already part-scoped, but `"moving"` is a fixed literal, not part-scoped).

**Layer 2 — deeper, confirmed by tracing the actual copy mechanism (this is why fixing Layer 1 alone will NOT fix the bug):** `MergeTreePartsMover::clonePart`'s non-zero-copy path (`MergeTreePartsMover.cpp:281`) → `IMergeTreeDataPart::makeCloneOnDisk` (`IMergeTreeDataPart.cpp:2440`) → `DataPartStorageOnDiskBase::clonePart` (`DataPartStorageOnDiskBase.cpp:677`, `src_disk->copyDirectoryContent(...)`) → generic `IDisk::copyDirectoryContent`/`copyThroughBuffers` (`IDisk.cpp:174-205`) → per-file `IDisk::copyFile` (`IDisk.cpp:63-79`) → `DiskObjectStorage::writeFile()` (`DiskObjectStorage.cpp:936-946`), which opens a **fresh `DiskObjectStorageTransaction` per file with `autocommit=true`** — there is NO shared transaction object threaded through the whole part clone (unlike `freeze()`, which does thread an `external_transaction` — `clonePart` has no equivalent parameter). `ContentAddressedTransaction::tryCreateWriteBuffer` (`ContentAddressedTransaction.cpp:631-661`) already KNOWS single-file autocommit is unsound for a part and explicitly throws `"Autocommit writes are not supported for content part files on a content-addressed disk"` for must-stay-blob files (`.bin`/`.mrk*`/`primary.idx`, `partFileMustStayBlob`, line 39-47) — but this guard does not cover the OTHER (non-blob) part files (`checksums.txt`, `columns.txt`, `count.txt`, etc.), which autocommit "successfully" as independent one-file manifests. Since a part always has ≥2 such files, the SECOND one's independent `promote()` finds the ref (currently the unscoped `"moving"`; even after a Layer-1 fix, the correctly part-scoped `"moving/<part>"`) already committed by the FIRST file's one-file manifest → the observed `ABORTED`/"already names a different committed manifest" exception. Reproduced live: `ALTER TABLE s36_move MOVE PART '0_0_0_0' TO DISK 'ca'` throws this on a completely FRESH pool, on the very first ever move, 2/2 setup verdicts passed beforehand (S36 run log `utils/ca-soak/tmp/s36_run2.log`, 2026-07-16T22:19:22).

**Conclusion:** `ALTER TABLE ... MOVE PART|PARTITION TO DISK|VOLUME <content-addressed disk>` is not merely mis-scoped, it is **architecturally unimplemented** for CAS destinations — the generic per-file cross-disk-copy mechanism MergeTree's part-mover relies on cannot produce the single-transaction, whole-part manifest CAS requires (exactly the same one-transaction-per-part model ordinary INSERT already builds correctly). A real fix needs a CAS-aware `clonePart`/copy override (mirroring the existing `disk->supportZeroCopyReplication()` special case already present in the SAME function, `MergeTreePartsMover.cpp:239`) that stages every file of the destination part into ONE `PartWriteTxn` and commits/promotes once — not a parser tweak. This blocks: **all of S36** (TO-CA is the very first leg; OFF-CA/chaos legs are never reached) and **S37's explicit-MOVE and TTL-MOVE-to-`cas`-volume legs** (same `clonePart`/`swapClonedPart` machinery, whether triggered by an explicit `ALTER ... MOVE` or the background TTL mover). S37's `max_data_part_size_bytes` leg (an oversized part routed straight onto the `ca` volume AT INSERT time via ordinary storage-policy volume selection, never touching `clonePart`) is a DIFFERENT code path and is expected to be unaffected — worth confirming separately. NOT fixed here (large/architectural, per the "STOP + escalate" protocol); reported to team-lead 2026-07-16.

**S37 run confirms + refines this (2026-07-16, 22/23 verdicts pass, only the chaos leg fails):** every leg that does NOT go through `clonePart` passes cleanly — `max_data_part_size_bytes` routing at INSERT time, the mixed-disk (2-disk JBOD) merge landing its output on `ca` (a MERGE writes a brand-new part via the ordinary build path, not `clonePart`), clean restart re-attach, and the explicit `MOVE ... TO VOLUME 'hot'` bringing a part back from `ca` to `local1` (an **OFF-CA** move — the destination disk is plain `local`, so it never touches the broken CAS-destination autocommit path; this direction is unaffected) all PASS. The card's TTL-MOVE-to-`cas` leg also shows PASS, but the run's own observations show the part was **already on `ca` before `MATERIALIZE TTL` ran** (`ttl_placement_before_ttl` == `ttl_placement_after_ttl` == `{"ca"}`) — because the TTL rule (`ts + INTERVAL 1 SECOND`) is already satisfied at INSERT time (`ts = now() - 10`), MergeTree picks the destination volume directly at part creation, so this leg never actually exercises `clonePart` either; it is a vacuous pass, not evidence the background TTL-mover path works. The one REAL local→CA relocation attempted in the whole S37 run — the chaos leg's `ALTER TABLE s37_ttl MOVE PARTITION ID 'all' TO VOLUME 'cas'` on a part still sitting on `local1` — throws a DIFFERENT symptom of the SAME Layer-2 bug: `Code: 48, NOT_IMPLEMENTED: "Autocommit writes are not supported for content part files on a content-addressed disk"` (the explicit must-stay-blob guard, `ContentAddressedTransaction.cpp:654-655`, firing cleanly this time instead of the non-blob-file ref-collision `ABORTED` seen in S36 — which exact exception you get is just an accident of which file the generic per-file copy reaches first). So: two different call sites, two different error codes, one confirmed root cause. Separately, minor/unconfirmed: the chaos leg's `rows_after == ttl_rows` assertion (`s36_s37_disk_move.py`, S37 chaos leg) compares against the fixed `ttl_rows` param (100) rather than the true expected total after two inserts into the same un-truncated table (100 + 100 = 200) — this looks like an off-by-existing-table-content bug in the scenario oracle itself, independent of the CAS bug, but is confounded by the mover's real failure here so not yet independently verified.

## Deployment nuance: a CA disk's default scratch path needs its `disks/` parent writable by the server uid (found via S36/S37 multidisk harness bring-up, 2026-07-16)

A `content_addressed` disk with no declared `<scratch_path>` lazily creates its local scratch dir at `<data-path>/disks/<name>/cas_scratch/` at server startup, run as the non-root `clickhouse` uid (`MetadataStorageFactory.cpp:231-238`, default = `getPath()/disks/<name>/cas_scratch/`). If the SAME storage config also declares a sibling `local`-type disk with an explicit `<path>` under that same `<data-path>/disks/` tree (e.g. `/var/lib/clickhouse/disks/local1/`), the official ClickHouse docker image's `entrypoint.sh` (`manage_clickhouse_directories`) pre-creates that sibling disk's declared path via `mkdir -p` running as ROOT, then `chown -R` **only the leaf directory it was told about** — never the shared `disks/` parent it implicitly created along the way. That leaves `disks/` root-owned/`0755` and unwritable to the non-root server process, so the CA disk's own later `mkdir -p disks/<name>/cas_scratch` throws `filesystem_error: Permission denied` and the server exits (233) before ever listening. Not a ClickHouse C++ bug (the fail-closed exception is correct behavior) — a config/deployment nuance: keep any sibling local disk's declared `<path>` OUTSIDE the CA disk's `disks/<name>/` namespace (e.g. under a separate `local_disks/` top-level dir) so the image entrypoint never pre-locks the shared parent. Fixed in the ca-soak harness at `utils/ca-soak/configs/storage_conf_multidisk_ch{1,2}.xml`.

## PRODUCT FINDING (#37, validated 2026-07-16): merge "progress reset in loops" under sustained S3 fault = mount-fence loss + ABORTED-defeated backoff (NOT a merge-vs-insert retry gap)

Investigated rigorously (code-map + s3faultproxy reproduction + TWO independent strong-model consults, opus+fable). The ORIGINAL framing ("INSERT survives by re-streaming from scratch, MERGE reruns the whole merge") is REFUTED: under a sustained S3 fault a plain INSERT dies with the byte-identical `Code 236 ABORTED stageManifest ... UNCERTAIN (retry budget exhausted)`. The in-commit CAS upload-retry stack is shared and caller-agnostic; there is no merge-vs-insert difference at the CAS layer.

MECHANISM (confirmed by both consults, log-verified on HEAD 3af7fa4dce3, s37 repro): sustained faulting fails the CAS write MOUNT-LEASE renewal PUT; the lease deadline expires / fence trips → every write (merge AND insert) fails `stageManifest`'s `fence_ok()` gate INSTANTLY (pre-attempt reject in `putIfAbsentControlled`, CasRequestControl.cpp:293-294 → mapped to the misleading "retry budget exhausted" text at CasPartWriteTxn.cpp:797 though nothing was attempted). Self-remount recovery is blocked by the same fault → the write outage persists the whole fault window; on fault clear, self-remount recovers (~16s) and the next merge commits cleanly. FAIL-CLOSED-CORRECT: no data loss, self-heals. The merge-specific SYMPTOM (239 full recomputes of an 805 MiB merge in 7.5 min) is the replication scheduler tight-looping.

THREE CA-SIDE DEFECTS (all backlog; no correctness/release blocker; all fixable without upstream coupling and WITHOUT architectural staged-part preservation):
1. OVER-FENCING (root amplifier). `SingleWriterSlot::backgroundLoop` (CasServerRoot.cpp:995-1025) + `renewOnce` (CasServerRoot.cpp:913-931) burn the whole writer incarnation on the FIRST transient renewal-PUT exception: `renewOnce` calls `backend->putOverwrite` directly (single-shot; NOT via the 90s controller), and `backgroundLoop`/`onRenewFailed` (:754-763) treat every exception identically and stop the loop forever, conflating a transient network fault with foreign-token supersession (the throw at :926 precedes the mismatch check at :927-928, so no mismatch was actually classified). FIX: on a `renewOnce` exception, RETRY on subsequent beats while the lease deadline is still valid; fence-now only on a real `onRenewMismatch` (token mismatch = proven supersession) or when the deadline actually nears. → any S3 blip shorter than the ~30s lease TTL becomes a non-event (no incarnation recycle, no cluster-wide write fence). CA-code-confined.
2. ABORTED DEFEATS THE EXISTING MERGE BACKOFF. Upstream `ReplicatedMergeTreeQueue::getPostponeTimeMsForEntry` (ReplicatedMergeTreeQueue.cpp:1614-1643) already implements `2^num_tries` backoff (cap `max_postpone_time_for_failed_replicated_merges_ms`, default 60s), but it never engages because CAS throws `ABORTED` and `ReplicatedMergeMutateTaskBase.cpp:71-77` special-cases `ABORTED` ("Interrupted merge … is not an error") → `need_to_save_exception=false` → `updateLastExeption` never called → `last_exception_time_ms=0` → postpone=0 → 239 recomputes instead of ~15-20. FIX: at the merge-facing CA commit boundary (or only in the fence-lost/Unresolved branch), throw a code OUTSIDE the ABORTED/`PART_IS_TEMPORARILY_LOCKED` exemptions for the retry-later class so the EXISTING backoff engages. CAUTION: `ABORTED` is load-bearing across CAS internals (putBlob outer 8× catch CasPartWriteTxn.cpp:203-225; CasPlainObjects contention :40/:64; relink boundary ContentAddressedMetadataStorage.cpp:1307) — translate only at the boundary and audit catch sites; genuine merge cancellation (shutdown/DROP/merges_blocker) must remain ABORTED-exempt.
3. OPACITY. (a) The "retry budget exhausted" message is emitted when nothing was ever attempted (fence-lost pre-attempt reject) — distinguish fence-lost from genuine 16-attempt/90s exhaustion at the throw site (this opacity plausibly fed 3 prior wrong analyses). (b) The 239 failures logged at Information level only; `entry->exception`/`last_exception` never populated → the storm is ~invisible in `system.replication_queue`. FIX: populate the exception + raise the log level for the retry-later class.

CORRECTNESS FOLLOW-UP (not verified in the repro): "no orphans" is counter-SUPPORTED (CasBlobPut=15 whole run; fence rejects pre-attempt so recomputes write nothing to S3) but NOT fsck-verified — no fsck/GC-to-fixpoint ran post-recovery. Verify with an fsck-to-fixpoint after a fence-loss recovery; the precommit-window edge (fence loss between `precommitAdd` and publish) links to the known S30 DANGLING-PRECOMMIT gap. Blast radius under a real sustained outage: the loop burns background merge-pool slots (starves merges of unrelated LOCAL tables on a mixed CA+local server) + ~190 GB local scratch in 7.5 min (scales with merge size × outage duration) + re-GETs source blobs adding load to the already-throttled S3.

PRIORITY: fix #1 first (kills the common transient-blip case + shrinks the sustained case); #2 makes any residual sustained outage back off (≤1 recompute/min) instead of storming; #3 is cheap and aids future diagnosis. Repro harness + logs: `utils/ca-soak/docker-compose-s3faultproxy.yml`, `utils/ca-soak/tmp/s37_repro_*`.

## VERIFY (not implement): CA↔CA same-pool MOVE PART/PARTITION (S37 3-disk), after MOVE-to-CA lands (noted 2026-07-17)

The MOVE-to-CA fix (spec `2026-07-17-cas-move-to-ca-design.md`) targets local↔CA. Moving a part between two CA disks in the SAME pool (the S37 3-disk `ca_local3` variant) is expected to LIKELY work via the same generic L1+L2 code with no special-casing: the moved part's content is already in the pool, so the target publish dedup-resolves the blobs (near-free) and is effectively a ref repoint. UNVERIFIED interaction: whether the target's final ref `<part>` collides benignly with the source's existing ref `<part>` (both namespaced by the table uuid) — or whether same-pool CA↔CA needs the source ref dropped before/as the target publishes. TODO after MOVE-to-CA lands: run the S37 CA↔CA-move leg; if it passes → close; only add special handling if it shows a real collision. NOT a blocker for the local↔CA cut.

## Killed mid-`MOVE PARTITION` leaves a persistent duplicated part (S37 chaos leg; pre-existing, NOT an R2 regression) (found 2026-07-17)

CONFIRMED (log + `system.parts`-verified, S37 chaos leg, HEAD a2c420d1fd2): hard-killing a replica mid-`ALTER TABLE ... MOVE PARTITION ID 'all' TO VOLUME 'cas'` leaves the partition DUPLICATED after heal — `SELECT count()=200` but `uniqExact(id)=100` (every row present twice), consolidated into ONE active part (`all_0_2_2_1`, ca, 200 rows, level=2) by a later merge, so it does NOT self-heal (a merge does not dedup rows). S36's single-part `MOVE PART` chaos leg IS atomic/green (rows=300, one copy) — only the multi-generation `MOVE PARTITION` policy/TTL path under kill duplicates.

ATTRIBUTION (honest): PRE-EXISTING, not caused by the MOVE-to-CA (R2) fix. The pre-R2 baseline (`runs/20260716T203706_S37_seed1`) showed the SAME duplication (`rows=200`) but split `disks={ca,local1}` via the L2 `NOT_IMPLEMENTED` half-failure. R2 fixed the move itself (`mover_error=None`, both copies now land on `ca`) but did NOT introduce the duplication. S37 pass count is unchanged at 22/23 across R2 (no regression; the OTHER previously-red leg — clean-restart placement — was fixed this round by the s37_ttl TTL-neutralization, commit a2c420d1fd2).

LIKELY GENERIC, needs classification: `MOVE PARTITION` under a hard kill is plausibly a generic ClickHouse crash-atomicity / replication-replay duplication (the move is a local per-part operation; a kill mid-move + queue/replica replay can apply it twice), NOT the CA `moving/`-prefix recovery (whose single-part path — S36 — is atomic). TODO: repro `MOVE PARTITION` + hard-kill on a NON-CA multi-disk policy to confirm generic; if generic → upstream/known-limitation, if CA-specific → investigate the `moving/`-ref restart recovery promoting a stale staging part. Chaos-edge (hard kill mid-move); does not block the local↔CA MOVE-to-CA feature (correct on all non-chaos paths + single-part chaos). Evidence: `utils/ca-soak/scenarios/runs/20260717T005228_S37_seed1/` + `.../20260716T203706_S37_seed1/` (baseline).

## Gate-filter gap: `Cas*:CA*` silently excludes ~60 RefWriter*/RefTableCacheEviction/RefSnapshotCodec tests (found 2026-07-17, R3)

The gtest filter `Cas*:CA*` used as the campaign's "battery stays 0-failure" gate does NOT match several suites in gtest_cas_ref_writer.cpp / gtest_cas_pool.cpp whose names start with neither "Cas" nor "CA": `RefWriterAppendLane`, `RefWriterRemount`, `RefWriterNamespaceRemoval`, `RefWriterRecoverySeal`, `RefWriterNamespaceBirth`, `RefTableCacheEviction`, `RefSnapshotCodec`, `ContentAddressedLog*`, `CountingBackendShape*` (~60 tests). CORRECTED comprehensive filter (use for ALL future gates):
`Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*`
Verified this round: R1 (COW map) and R2 (MOVE-to-CA) did NOT break any excluded test — implR3's corrected-filter sweep on the combined HEAD is 907/908, and R1's diff provably does not touch the seal/decompress/recovery path (only excluded failure is the pre-existing one below). So the gap did not hide a regression here, but it MUST be closed for future gates.

## Pre-existing: RefWriterRecoverySeal.SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery zstd-fragile (found 2026-07-17)

`RefWriterRecoverySeal.SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery` (gtest_cas_ref_writer.cpp:2916) fails with zstd "Src size is incorrect" in the second `listRefs` (the corrupt-foreign-seal ADOPT retry). NOT an R1/R2/R3 regression (log + diff verified): R1 did not touch the seal compress/decompress or `decodeRefTableSnapshot` path (its snapshot bytes are byte-identical), and implR3 confirmed it fails without R3's ABORTED->NETWORK_ERROR reroute. The test's OWN comment flags the root: it depends on a corrupt-but-decodable seal being adopted ("trailing garbage tolerated by decodeRefTableSnapshot"), which it labels a separate open finding "F3-1a side-finding" (rev6 findings doc) — the corrupt-seal-adoption decodability is fragile (zstd rejects the injected corruption as an undecodable frame rather than tolerable trailing garbage). This test pins the no-wedge/restartability contract, not the decode laxity. TODO: either harden `decodeRefTableSnapshot`/seal-adopt to reject-cleanly (don't adopt a corrupt seal — fail-closed re-derive) per F3-1a, or fix the fixture's corruption injection to produce genuinely-tolerable trailing garbage. Test-health / F3-1a follow-up, not a campaign blocker.

## *** CRITICAL / RELEASE-BLOCKER: acked-then-lost INSERT data-loss on cross-request retry after R3's stageManifest Unresolved→NETWORK_ERROR (found 2026-07-17, R4 soak) ***

CONFIRMED (independently verified, not inference): under a chaos fault (S3 ambiguity) a client-retried sync INSERT can return server SUCCESS (`QueryFinish`, HTTP 200) while its rows NEVER land — silent data loss. fsck stays CLEAN (dangling=0/unreachable=0/unaccounted=0) and both replicas AGREE, so the CAS integrity oracle does NOT catch it; it only shows as a row-count deficit.

EVIDENCE (R4 phase-3 soak, seed=42, chaos overlapping rustfs-pause 46s + ch2 kill 41s; build/r4_soak.log; run cluster ca_stress table):
- CHECKPOINT FAILURE: node1 model=632224 got=631106, deficit=1118.
- 9 op_ids absent from BOTH replicas: 13055(n=110) 13057(29) 13059(161) 13060(119) 13061(152) 13062(120) 13064(155) 13066(188) 13067(84); sum = 1118 = EXACT deficit.
- Airtight acked-vs-errored proof: the soak model applies on SUBMIT (run.py:263-272 _submit_insert); the driver RE-RAISES on retry-exhaustion (retry_on_transport, cluster.py:442-443) → the run ended in CHECKPOINT FAILURE (count), NOT WORKLOAD FAILURE (drain re-raise) → therefore NO insert future raised → every submitted INSERT (incl. every reroute-retry) got a final HTTP-200 success. 9 of them are absent ⇒ their success was a false dedup-no-op.

MECHANISM (grounded; exact C++ trace = the follow-up): the driver's retry idempotency assumes B138 — "a FAILED sync insert leaves NO block-dedup token, so a byte-identical retry truly re-inserts" (run.py:_insert_with_retry docstring). R3 (#37) changed the `stageManifest` Unresolved / ambiguous-PUT abort from an internal, same-request ABORTED retry loop to a client-visible `NETWORK_ERROR` "retry-later" (CasPartWriteTxn.cpp:796-799), forcing the soak driver to reroute a FRESH HTTP request with the byte-identical INSERT. If the first (failed) attempt registers an RMT block-dedup token (Keeper) BEFORE/without the CAS manifest durably committing, the cross-request retry dedups against that ORPHAN token → success, no write → loss. R3 introduced/exposed this by moving the ambiguous-abort class ACROSS the request boundary (pre-R3 the internal retry re-committed the same block within one request/dedup scope). The CAS layer's own comment (CasPartWriteTxn.cpp:791-793) claims the Unresolved path leaves "inert unreferenced debris" (true at the CAS layer — hence fsck clean); the loss is a layer up, at the ReplicatedMergeTreeSink block-dedup-token vs CAS-commit ORDERING/atomicity.

FIX DIRECTION (needs a hard decision — do NOT land blind): (a) make the RMT block-dedup token durable ONLY after the CAS manifest is durably committed (atomic ordering) so a failed attempt leaves no token; or (b) roll back the dedup token on a stageManifest-Unresolved abort; or (c) R3 refinement — the AMBIGUOUS (may-have-committed) Unresolved case must NOT be handed to a cross-request client retry (keep it an internal same-request retry, distinct from a clean fence-lost-definitely-didn't-commit case which is safe to retry cross-request). Requires a source trace of ReplicatedMergeTreeSink dedup-token timing relative to the CAS stageManifest commit. Until fixed, R3 (#37) MUST NOT be considered ship-ready — it trades an availability fix for a fault-conditional silent data-loss. Repro: utils/ca-soak `python3 -m soak.run --phase 3 --duration 20m --seed 42 --insert-mode sync` on docker-compose.yml (the chaos leg fires the ambiguous-PUT fault).

### UPDATE 2026-07-17 (dedupTrace source review): the "orphan dedup token" MECHANISM above is REFUTED; empirical data-loss STANDS, mechanism now OPEN

An independent read-only source trace (ReplicatedMergeTreeSink::commitPart) refutes the stated mechanism: the CAS commit (`transaction.renameParts()` → `stageManifest`, ReplicatedMergeTreeSink.cpp:976) runs STRICTLY BEFORE the persistent `/blocks/<block_id>` dedup-token create, which is in the single atomic Keeper multi at :985 (token appended via StorageReplicatedMergeTree::getCommitPartOps, StorageReplicatedMergeTree.cpp:9704-9708). A `stageManifest` NETWORK_ERROR throw at :976 means :985 is NEVER reached → NO dedup token is created for a failed CAS write, and no internal retry re-reaches :985 (stage_switcher rethrows unconditionally; ZooKeeperRetriesControl only retries KeeperException/hardware errors, not a DB::Exception(NETWORK_ERROR)). So a byte-identical retry finds NO token to dedup against → it should truly re-insert. block_id IS content-hash based (InsertDeduplication.cpp:383-475), so IF a token existed a retry would collide — but none exists on the failed-CAS-write path.

R3-causality refined: airtight for `putBlob`/`uploadFromSource` (post-#37 NETWORK_ERROR escapes putBlob's ABORTED-only 8x loop on attempt 1, vs pre-#37 internal 8x retry — CasPartWriteTxn.cpp:205-225); but for `stageManifest` itself R3 changed only the error CODE, and at the sink level ABORTED vs NETWORK_ERROR propagate out of commitPart identically. So R3 did NOT introduce a server-side token-leak.

STATUS: The EMPIRICAL data-loss is CONFIRMED and REAL (9 acked ops, sum=1118=exact deficit, absent both replicas, verified). But the ROOT-CAUSE MECHANISM is OPEN — the orphan-token story is wrong. Leading (UNCONFIRMED, untraced) hypotheses for the actual trigger: (i) reroute-across-replicas under the ch2-kill: a first attempt that reached :985 on ch2 (creating the token + ch2 part-znode) then had its ack lost to the kill, so the client reroutes to ch1 which dedups at the ephemeral-lock check-not-exists (createEphemeralLockInZooKeeper, EphemeralLockInZooKeeper.cpp:52-97) against the durable token and returns success without the part being recoverable on either replica; (ii) client-side/soak-driver retry classification (NETWORK_ERROR auto-retried where ABORTED was not) re-issuing inserts into a dedup-eligible state. NEEDS an instrumented, isolated repro (single-writer, controlled ambiguity + reroute) to nail — armchair source reading cannot resolve it. R3/#37 remains NOT ship-ready pending this (the loss reproduces on the R3 binary regardless of exact mechanism). Do NOT attempt a fix until the mechanism is established.

### UPDATE-2 2026-07-17: data-loss now REPRODUCED + TRACED — see docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md (commits ef8eda50316, a132e6fc6c9)
Mechanism is no longer "open": build/dl_probe.py reproduces it deterministically (pause rustfs>90s budget + kill ch2 during continuous sync inserts → ~15% acked-but-absent). text_log/part_log/blob_storage_log trace shows: a part's block_id dedup znode + part-znode commit to Keeper, but its CAS blob PUTs fail (S3 outage, 112 failed Uploads) on a then-killed replica → part is RemovePart'd (data absent both replicas) WHILE the block_id dedup znode survives → the byte-identical R3-NETWORK_ERROR client retry dedups against it ("already exists on other replicas ... ignoring it") → acked, silently lost. Core defect: block_id dedup-znode lifecycle decoupled from CAS-blob durability (generic RMT assumes committed-block data is durable on the committer's LOCAL disk; on CA it's shared S3 that can fail to persist while Keeper commits). R3 amplifies (turns the ambiguous case into a client retry) — introduced-vs-amplified pending a pre-R3 repro. R3/#37 still NOT ship-ready.

### UPDATE-3 2026-07-17: mechanism RE-TRACED against source + CORRECTED (commit f7d337bab5b) — durability ordering is INVERTED on CA
A line-by-line code re-trace (prompted by a user challenge to a contradiction in the report) CORRECTS the write-path claim. Earlier notes said `renameParts()`/`stageManifest` at ReplicatedMergeTreeSink.cpp:976 is the durable CAS commit that runs BEFORE the Keeper multi (:985), so a failed CAS write throws before any znode — FALSE. Verified ordering in `commitPart`: (1) `renameParts()` (:976) on CA = **pure overlay re-key, NO publish, blobs B188-deferred** (`ContentAddressedTransaction.cpp:368-369,1177`; blobs "do NOT upload here" :589); (2) **Keeper multi (:985) commits block_id+part-znode durably** (Keeper is a separate container, UP under rustfs-pause → clean ZOK); (3) `transaction.commit()` (:990) → `MergeTreeData::Transaction::commit` (MergeTreeData.cpp:8797-8799 `commitTransaction`) → `ContentAddressedTransaction::commit`→`publishStaging`→**`uploadPendingBlobs`+`promoteBuild` = the ONLY durable point (:358/:361)**, hangs 90s on paused rustfs → THROW → txn destructor `rollback()` (MergeTreeData.cpp:8689) = "Undoing transaction … Removing parts" (local part removed). So on CA the durability order is **INVERTED**: Keeper metadata at :985 commits BEFORE part data at :990 — a genuine **split-commit window :985→:990**. On a plain disk this is safe (data already durable on the committer's LOCAL disk before/at rename, part re-fetchable); on CA `renameParts` makes nothing durable, so the RMT protocol's "renamed part = durable data" assumption is false. The surviving block_id (independent `replicated_deduplication_window` lifetime) makes the byte-identical retry a false dedup no-op (`getActiveContainingPart`→null → `exists_locally=false` → "already exists on other replicas as part …; ignoring it", ReplicatedMergeTreeSink.cpp:511-517 → INSERT_WAS_DEDUPLICATED → ack, 0 rows). R3 governs SILENCE only (its NETWORK_ERROR "retry-later" induces the retry); the split-commit window is generic-CA. FIX = HARD product/arch decision (do NOT land blind): (a) verify-on-dedup on CA — honor a block_id dedup only when the referenced part's data is verified recoverable; and/or (b) gate the Keeper block_id/part-znode commit on CA-data durability (publish data before/atomically with the multi — touches RMT commit ordering); and/or (c) invalidate the block_id znode when its part is rolled-back/LOST. Pre-R3 repro still only needed to classify introduced-vs-amplified (with R3 = silent loss; pre-R3 the same split-commit likely surfaces as a hard client error = not silently lost). Full section: report "CORRECTION (2026-07-17, code re-trace)".

## [B199] FIXED 2026-07-17: `RefWriterRecoverySeal.SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery` deterministic RED — test fixture invalidated by formats-v3 zstd framing
RESOLVED — RCA `docs/superpowers/reports/2026-07-17-b199-recovery-seal-zstd-rca.md`. NOT a product bug: the fixture faked the "foreign different object" as `own bytes + trailing garbage`, which the pre-v3 text decoder tolerated (the flagged F3-1a decode laxity) but the v3 zstd frame check correctly rejects (`Src size is incorrect` = fail-closed on a genuinely undecodable seal — the frame check CLOSED F3-1a). Fix (test-only): fixture gains `corrupt_foreign_bytes` (a caller-provided VALID foreign object); the test lands a real 3-row foreign seal and asserts adoption via `listRefs()==3` (provably the foreign content, not a local 2-row recompute). Battery after fix: 907/907, zero reds. Historical entry below.
Deterministic (5/5 + clean-dir rerun) on the current branch. Signature: the test's seal PUT conflict resolves as expected (CORRUPTED_DATA from `resolveByExactGet`, "observed a DIFFERENT object"), but then the recovery `listRefs` -> `ensureRefTableRecovered` dies with `CAS cas_ref_snap: zstd decompression failed: Src size is incorrect` (gtest_cas_ref_writer.cpp:2944; CasRefLedger.cpp:383/:163). NOT caused by the renameParts durability fix: byte-identical failure present in `build/fix2_cas_battery.log` (2026-07-17 03:58, R3-era binary, BEFORE T1/T2 existed) — that nightly battery RED went unnoticed (process lesson: battery log analysis must assert FAILED==0, not skim). Suspect area: rev.6 seal / snapshot-streaming codec interplay (seal-conflict path leaves a `_snap` object the recovery reader can't decompress — truncated/misframed write on the conflict path?). Needs its own systematic-debugging pass; not a T2 gate (pre-existing), but IS a real recovery-path red — triage soon, before the next release gate.

## [B208] CA startup mount-probe is fail-closed against a TRANSIENT S3 outage — server aborts and stays down (found by S40, 2026-07-17)
A server started while the object store is unreachable dies during metadata load: the CA-pool mount startup capability probe (S3 write `_probe/<uuid>/token`) times out (`WriteBufferFromS3 ... Timeout`) -> `Application::main` treats it as fatal (`Caught exception while loading metadata: Code 499 S3_ERROR`) -> exit 243, no retry, stays down until an operator restarts it. Seen twice: S40 run `20260717T090957_S40_seed1` (ch2 `docker start` inside the rustfs pause window — by design of the scenario), and the earlier dl_probe repro (same Exited 243 after `docker restart -t 1` during the pause). Product question: a bounded startup retry / degraded-start (mount later, serve non-CA tables meanwhile) instead of aborting — weigh against fail-closed principles (a server that starts without its pool must not fake readiness). NOT a gate for the durability fix (S40's contract only requires acked data to survive on the live replica — it does). Related: the self-remount recovery work covers RUNNING servers losing the pool; this is the STARTUP window. NOTE (T3 review): S40's fixed fault schedule hits this abort DETERMINISTICALLY (ch2 restarts inside the pause) and the card carries no verdict on ch2 rejoining — when B208 is fixed, add an informational recovery verdict to S40 so a regression here produces signal.

## GREEN-DEBT: S39 ci-scale config bug — `short_fault_s=15 >= _MOUNT_RENEW_PERIOD_S=10` invariant violation at ci scale (found DL-fix T4, 2026-07-17)
S39's ci param row bakes in a short-fault window that violates the card's own timing assumption (the "short" leg must complete its fault inside a renewal period for the no-fence assertion to be sound); dev scale is correct and is what all prior green runs used (ci scale had NEVER run per RUN_HISTORY — this red was latent since the card's creation e93c28a17694a1). Fix the ci row in `s39_lease_fault_tolerance.py` param_table (size ci off the same 30s TTL anchor as dev, keeping short < renew period) and run S39 at ci scale to green. Per the no-known-reds rule this is a tracked return-item, not a "known".

### FIX LANDED 2026-07-17: generic renameParts disk-transaction close (spec 2026-07-17-part-durability-before-keeper-commit-design.md)
`77484196b0d` closes every part disk-storage transaction in `MergeTreeData::Transaction::renameParts` (part durable BEFORE the Keeper block_id registration; disk commit moved off the `data_parts` lock). Regression test `05014_insert_dedup_disk_commit_failpoint` + targeted failpoint `part_storage_fail_commit_transaction` (`2c1b15ed4ae`): pre-fix count=0 (silent loss), post-fix count=1. Gates, ALL GREEN on the fixed binary: S40 (new permanent card, `e302c36421f`) PASS 10/10 with acked=3796 lost=0; dl_probe (tracked `utils/ca-soak/tools/dl_probe.py`) LOST=0 (pre-fix ~198/1314); S39 dev 11/11; S36 26/26; 20m seed-42 soak (the original R4 chaos recipe that lost 1118 rows) PHASE3 OK, checkpoint deficit ZERO, dangling=0. S37 22/23 = card-oracle artifact (GREEN-DEBT, see the VERIFY entry). **R3 (#37) ship-readiness RESTORED.** Residual narrower hazard (block_id outliving a durably-committed part lost later) stays out of scope — verify-on-dedup is the candidate if it ever matters. Upstream submission draft: `tmp/upstream_issue_dedup_durability.md` (pending user decision).

## GREEN-DEBT: local build-dir config drift — ALL localize_rust_c_* rules in build/build.ninja lost their reference-library args (found 2026-07-17 at image build)
`ninja -C build clickhouse` now fails at `localize_rust_c_chdig` ("Error: no reference libraries given"); inspection shows EVERY localize rule (chdig, polyglot, wasmtime, delta_kernel_ffi) in the generated build.ninja carries only 4 args (lib/ar/objcopy/nm) and zero refs — while corrosion-cmake's generator only creates these targets when `_localize_ref_libs` is non-empty (contrib/corrosion-cmake/CMakeLists.txt:319-415). So some past reconfigure generated with a state where the genexprs produced nothing — latent until today because the rust targets were never dirty in recent incremental builds (T2's build passed because its cargo steps rebuilt but chdig localize did not re-run... first execution today failed). Impact: any future `ninja` touching rust contribs fails in build/. Fix on resume: full cmake re-configure of build/ (then check one localize rule has refs) — and understand WHICH configure produced the argless state (guard against recurrence). The 2026-07-17 nightly image was built from the T2 binary (10:35, all DL-fix gates ran on it) — unaffected.

## GREEN-DEBT: ca-soak GC-checkpoint timeout formula assumes normal-speed GC throughput -- blows the budget under TSan (found 2026-07-18)
`soak/checker.py:fixpoint_timeout_s` computes a backlog-scaled real-time bound as `5 * (initial_unreachable/reclaim_per_round_guess=50) * gc_interval_s`, i.e. it assumes the SERVER's own background-GC round throughput is roughly 50 reclaims/round (a normal-speed baseline). Running the standard 20-min phase-3 chaos soak against a TSan-instrumented server hits `CHECKPOINT FAILURE: GC unreachable count never stabilized within {bound}s` at the `gc_checkpoint` stage -- the unreachable-count history was clearly trending down (peak 26170 -> 12251) when the budget expired, i.e. GC was actively converging, just slower than the formula assumes; `dangling=0` and fsck stayed settled throughout. This is a harness-timing artifact, NOT a correctness bug -- TSan's severe per-memory-access instrumentation overhead drops the server's actual reclaim throughput well below the formula's baseline, so a backlog that comfortably fits the budget under a normal or ASan-instrumented binary blows through it under TSan. Same root-cause class as `CasPartWriteTxn.ManifestCapEncodedBytesJustUnderStagesSuccessfully` above (TSan overhead vs a real-time budget calibrated for normal speed; that gtest is now FIXED by freezing its clock, but this soak-harness formula is a different codepath and still needs its own fix). Fix on resume: add a sanitizer-aware multiplier to `fixpoint_timeout_s` (or an explicit CLI override) so a full TSan chaos-soak can run through the chaos window; low priority since the workload/mutation/ttl_pressure stages and the full gtest battery already validate TSan correctness with zero races found.

## RESOLVED 2026-07-18: the CAS gtest battery no longer needs a known-abort exclusion list at all
Historically, running the CAS gtest battery under ASan/TSan required a peel-and-continue script
(`build/asan_battery.sh` etc.) that accumulated a 41-entry list of tests known to abort the whole
process (a `LOGICAL_ERROR` throw calls `abort()` under `DEBUG_OR_SANITIZER_BUILD`, per
`Exception.cpp`'s `handle_error_code`), excluding each by name. On the user's explicit directive
("почини раз и навсегда, без всяких странных списков исключений" — fix it once and for all, no more
exclusion lists), audited every test in that class and closed all of them:
- 3 genuine stack-use-after-scope bugs (not the LOGICAL_ERROR class at all — a red herring the
  exclusion list had been silently papering over) plus 2 latent ones of the same kind: an event-sink
  capture vector declared after the Pool. Fixed by reordering declarations (`99879af4aca`).
- 3 test-only fault injections that misused LOGICAL_ERROR to simulate an ordinary external/observer
  failure (a sink callback throwing, a construction failure, an "unrecognized exception" example) —
  swapped to `UNKNOWN_EXCEPTION` (`4efc898b951`, plus the CI-fix commit `def79031982`'s B122 case using
  `CORRUPTED_DATA`).
- 2 production sites that genuinely misused LOGICAL_ERROR for expected external/data failures
  (OpenSSL/allocation faults in `CasBlobHashingWriteBuffer.cpp`, a decode-reachable data-integrity
  check in `CasRefSnapshotFormat.cpp`) — swapped to `OPENSSL_ERROR`/`CANNOT_ALLOCATE_MEMORY` and
  `CORRUPTED_DATA` respectively (`0e069357957`).
- 6 tests exercising genuine production invariants that correctly throw LOGICAL_ERROR — split each
  into the existing release-build assertion (`#ifndef DEBUG_OR_SANITIZER_BUILD`) plus a new
  death test (`EXPECT_DEATH`, `#if DEBUG_OR_SANITIZER_BUILD`) that proves the abort positively instead,
  matching the pre-existing `CasBlobDigestDeathTest` precedent (`99879af4aca`, `0d5f0be10c5`).
- A second gtest-filter coverage gap (`CaWiring*`/`CaTransaction*`/etc., ~89-90 tests, matching neither
  `Cas*` nor `CA*`) that had hidden 3 of the above bugs from every battery run this session — see
  `reference_ca_gtest_gate_filter` memory / `def79031982`.

**Result**: `unit_tests_dbms --gtest_filter='<the corrected filter>'` (no `:-exclusions` at all) now
passes 1034/1034 under ASan, 1034/1034 under TSan, and 1030/1030 under a plain (non-sanitizer) build
(the count differs only because 4 `#ifdef DEBUG_OR_SANITIZER_BUILD`-gated death tests exist solely in
sanitizer builds). Any CAS gtest battery gate going forward can drop the peel-and-continue exclusion
machinery entirely and just run the filter directly.

- [ ] CLEANUP (from F4a review 2026-07-21): delete dead pre-rev.6 config keys `content_addressed_allow_shared_pool` and `content_addressed_gc_grace_sec` from the ~7 integration-test XMLs that still set them, then drop both from `ContentAddressedSettings`' `non_cas_keys` skip-set so typo detection covers that namespace again. They are read nowhere in the current factory.

- [ ] CLEANUP (from final-review polish 2026-07-21): unify `content_addressed_garbage_collection_log`'s own `srid` column (and the `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` input-arg shorthand docs) with the spelled-out `server_root_id` naming F3 landed for `system.content_addressed_mounts`.
- [ ] DOC POLISH (from final-review polish 2026-07-21): `review1.md:147`'s bare "F1" tag collides with that same doc's own "finding N" numbering convention used everywhere else in it; and `refactoring-ideas.md:383` now anachronistically says the 2026-07-10 `01271_show_privileges` fix used the `SYSTEM CONTENT ADDRESSED GC RUN` row name, but that rename only landed in the 2026-07-21 F1 sweep — on 2026-07-10 the command was still `GARBAGE COLLECTION`.
- [ ] CHANGELOG (from final-review polish 2026-07-21): write the release-note/changelog line for the now-live unknown-CAS-config-key rejection (fails disk startup on a typo'd key; was previously a silent no-op) once the feature ships.

- [x] RESOLVED as misdiagnosis + REAL FIX LANDED (f1f11 soak 2026-07-21): the "post-kill CA table load takes minutes" finding was an artifact — the table sits in a lazy_load_tables=1 DB (706095958ea) and materializes in ~18 ms on first touch; nothing touched it post-kill, while SYSTEM SYNC REPLICA misreported the unmaterialized StorageTableProxy as "is not replicated". Fixed in 2ba28ac4b6f (unwrapTableProxy across single-table SYSTEM verbs + stateless test 05017). OPEN EMPIRICAL TAIL: measure post-fault getNested cost under churn at the next soak's first chaos checkpoint — if genuinely minutes, that is the real availability item.
- [ ] lazy_load_tables follow-ups (from T15 review, pre-existing): whole-db DROP REPLICA safety scan (InterpreterSystemQuery.cpp:~1687) and RESTART REPLICAS iteration skip unmaterialized proxies — a stale remote replica in ZK may stay uncleaned for lazy tables; STOP/START <action> on a single lazy table parks the ActionLock on the PROXY, invisible to the later-materialized nested storage.

## Ref-ledger follow-ups from the two-model adversarial consult (2026-07-21) {#ref-ledger-consult-followups-2026-07-21}

Consult-flagged, controller-verified, deliberately deferred with measurement/design gates. Evidence:
`docs/superpowers/reports/2026-07-21-reftablestate-experiments.md`, `tmp/consult-gpt56sol-answer.md`.

- **Post-durable-PUT allocation window in the ref-lane flush** — folded into the publish-confirm spec
  (`2026-07-23-cas-fetch-handoff-publish-confirm-design.md` §ledger-hardening) and tracked there, not
  here; this pointer stays only so the finding isn't rediscovered. Two round-2 nuances not to lose
  when restructuring: the catch's "permanently unreplayable" framing over-claims (the covered region
  can throw via `MemoryTracker` limits on a durable+applied transaction); wedge resolution followed
  by flush is a path `BM_FlushInstall` does not model yet — measure it before changing anything.
- **Recovery re-runs 3-4 codec passes per snapshot row** (measured, est. 2-3x recovery/GC-rebuild
  cut) — `recoverRefTableDetailed` decodes, `stateFromSnapshot` re-encodes+re-decodes (hand-built
  defense), then per-row size helpers re-encode again. Fix: a validated-witness type
  `decodeRefTableSnapshot` produces that `stateFromSnapshot` accepts without the round-trip.
- **`precommits` is a plain `std::set`, deep-copied per state scratch copy** — bounded only by the
  ~64 MiB admission byte budget, not the 1,000-op cap; every shipped "O(1) ~58 ns copy" benchmark
  used a one-precommit fixture. Do not build a third COW container without a number: extend
  `BM_ScratchCopy`/`BM_Admits` with a P-sweep (1/100/10,000) first.
- **GC per-table recovery gate before ref-log fold** (defense-in-depth; mandatory before any
  multi-writer or rolling-upgrade-skew milestone) — refuted as a live defect today (single
  lease-holder cannot mint the fabricated history this would catch), but still worth building before
  that changes. Fix shape: per-table `recoverRefTable(ns)` before folding new logs, `CORRUPTED_DATA`
  clamps the table (no cursor advance) rather than aborting the round.

## Disk-error (ENOSPC / inode-exhaustion) audit follow-ups {#disk-error-audit-followups-2026-07-21}

Staging/target/GC disk-error audit verdict held (staging ENOSPC fail-loud, Native S3 corruption-free,
GC decision-durable-before-delete). Residual gaps, ordered by value:

- **HARD: size guard at dedup-admit** — `PartWriteTxn::observeAndAdmit` never compares the observed
  size against the caller's expected size, so a truncated object at a content-addressed key can be
  admitted as a dedup hit, producing a durably unreadable part.
- **HARD: temp-file + rename in the local blob write path** — moved to §14, paired with the guard above.
- **DESIRABLE: fsck physical-size check for blob bodies** — `runFsck` HEADs every blob but never
  compares physical size against the expected size, so a truncated blob passes as `Reachable`; the
  listing already carries the sizes, so this is free.
- **DESIRABLE: free-space guard + orphan sweeper for `scratch_path`** — no `statvfs` check before a
  local staging write, and orphaned `*.tmp` files from an unclean restart are never swept (the S3
  staging prefix has a sweeper; local scratch does not).
- **MINOR: wrap the GC post-CAS cleanup in try/catch** — the post-CAS manifest-body delete loop and
  hand-off prefix wholesale delete aren't wrapped, so a genuine backend error escapes the round after
  its `gc/state` CAS already committed (data-safe, but reddens the round unnecessarily).
- **DESIRABLE: GC scheduler backoff + a distinct storage-full signal** — the pacing loop retries a
  failing round forever with no backoff and no ProfileEvent distinguishing target-storage-full from
  generic instability.
- **VERIFY: late-landing conditional PUT after fence loss** — same hazard class as the historical
  Late-Predecessor-PUT item; confirm successor-side `writer_epoch` gating rejects it, fold into rev.6
  lease work rather than tracking separately.
- **MINOR: destructor-`abandon` live-epoch precommit debris** — if `abandon` fails during a failed
  transaction's destruction, the live-epoch precommit binding persists until remount; bounded, but
  worth a periodic re-`abandon` retry under a persistently broken backend.

## `lazy_load_tables` / `StorageTableProxy` — feature-level decision needed (consult audit 2026-07-21) {#lazy-load-tables-decision-2026-07-21}

Third incident of the same class (unforwarded `IStorage` virtual / direct cast through the proxy):
SYSTEM verbs (fixed, 05017), action-lock parking (open), mutations (`checkMutationIsPossible`,
fixed + 05021). A commissioned audit
(`docs/superpowers/reports/2026-07-21-storageproxy-forwarding-audit.md`) found **~60 unforwarded
virtuals, ~45 of class "must forward"**, including a critical one: `backupData`'s no-op default
means a BACKUP of a not-yet-materialized lazy table silently contributes NO data. Design findings:
no compile-time guard exists for "new virtual not forwarded"; swap-on-materialize does NOT fix the
class (escaped `StoragePtr`s in the UUID map/action locks + two lock domains); the clean long-term
shape is catalog-entry laziness (real refactor). Consultant recommendation: the feature as
implemented is net-negative — disable/quarantine rather than fix one virtual at a time.

- [ ] **USER DECISION**: quarantine/disable `lazy_load_tables` vs fund the full remediation
  (complete forwarding sweep + Clang-AST CI guard + backup regression test) vs catalog-entry
  laziness refactor. Until decided: treat every new lazy-table symptom as this class first.
- [ ] THIRD bug of the class found while validating the mutation fix (2026-07-22): `MATERIALIZE
  TTL` through a lazy proxy fails with `INCORRECT_QUERY` "no TTL set" even after the
  `checkMutationIsPossible` forward — the proxy's cached in-memory metadata carries columns only
  (no TTL/ORDER BY), and `getInMemoryMetadataPtr` deliberately does not forward (audit class C).
  Candidate rule if the feature stays: forward metadata to nested ONCE MATERIALIZED (no laziness
  left to preserve at that point); needs its own consult.
- [ ] If the feature stays: forward at least `backupData`/`restoreDataFromBackup`/
  `supportsBackupPartition`/`finalizeRestoreFromBackup`, `onActionLockRemove`,
  `supportsOptimizationToSubcolumns` (the audit's three most-urgent), then the rest of class B.
- [ ] FOURTH bug of the class + a REVERT (2026-07-22, xhigh review): the `checkTableCanBeRenamed`
  forward added on `StorageTableProxy` (7ab1fc15f4c) was REVERTED — it materializes the lazy table
  (`getNested`) while `DatabaseAtomic` holds its non-recursive database mutex (DatabaseAtomic.cpp:321/346),
  and a schema-inferred lazy `Buffer` resolves its destination via `DatabaseCatalog::getTable` in its
  constructor (StorageBuffer.cpp:180), re-entering the same database and self-deadlocking (cross-database
  RENAME/EXCHANGE can hold two database mutexes across the same work). So the nested engine's rename
  restriction is once again bypassed for a lazy (never-accessed) table — the pre-existing gap is REOPENED,
  not newly introduced. Correct fix (same shape as the other class-C bugs): materialize the proxy BEFORE
  any database mutex is taken, at the interpreter level, then re-fetch/verify identities under the lock and
  run the check on the materialized storage. NOTE for any upstream PR: the KEPT generic `checkMutationIsPossible`
  forward on `StorageProxy` also changes `StorageTableFunctionProxy` semantics (a table-function proxy now
  answers the mutation-possibility check from its nested storage rather than the `IStorage` default) — sound,
  but call it out explicitly (codex F5).

## CAS disk lifecycle rev.8 round (FORGET-only) — closure + residuals (2026-07-23) {#disk-lifecycle-rev8-closure}

Round: spec `docs/superpowers/specs/2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md` (rev.8,
FORGET-only); plan `docs/superpowers/plans/2026-07-22-cas-disk-lifecycle-rev7.md` (17 tasks); problem framing
`docs/superpowers/specs/2026-07-22-cas-disk-lifecycle-problem-and-constraints.md` (goals G1–G7).

**Resolved this round:**
- **G4 / `05020` test isolation** — `05020_content_addressed_fsck` (+ the `04290`/`04295` family) now use a
  unique per-run disk name + pool path (plan Tasks 1–2). The old fixed-name registry entry that made a
  same-server retry reuse a stale disk and trip `throwNotMounted` on `GC RUN` is gone; the Dormant/UNMOUNT
  husk state it depended on was also rolled back (Task 15), so that failure mode no longer exists at all.
- **G1 abort / G2 zombie-spam (terminal case)** — the lease-loss six-class gate throws instead of aborting
  (G1's no-abort was Part 1, landed earlier); the GC scheduler now self-exits on `Vanished`/`IdentityLost`
  (whole-increment-review C1 fix, `1fe585ea078`), closing the eternal `CORRUPTED_DATA`-every-tick class.
- **G3 generic-code correctness** — the throw-when-uncertain gate + the empty-proof rule kill the
  silent-empty ATTACH (plan Tasks 5/8/9); **G5 FSCK-on-running** with the `meta_without_body` advisory (Task 13).

**NOT resolved (deliberately deferred):** the underlying **disk-lifecycle-leak** proper — a CA disk is still
cached forever in the disk registry (`Context::getOrCreateDisk`) with no teardown/eject on `DROP TABLE`, and
there is no runtime re-use of the same disk after a stop (G6 is met only node-locally via `FORGET`; G7
abandoned). The Dormant/UNMOUNT/MOUNT reuse machinery that pursued this was rolled back (spec rev.8 §9);
`FORGET` is the node-local decommission story. Full eject-on-`DROP` is future work (the disk-lifecycle
redesign; v2 door in git history).

**Accepted residuals / watch items (each a pointer into this round):**
- (a) **`search_orphaned_parts_disks=ANY` × a transient CA disk strands an unrelated table's load** —
  ACCEPTED (spec §4 blast radius). With `search_orphaned_parts_disks=ANY` the orphaned-parts sweep touches
  every disk, so a transient / `IdentityLost` CA disk makes an unrelated table's AsyncLoader load throw, and
  AsyncLoader does not retry-on-touch → the table stays FAILED until a manual `ATTACH`. Cure: `ATTACH` (or
  restart); guidance: keep `search_orphaned_parts_disks=LOCAL` when a CA disk may be transiently unreachable.
- (b) **Teardown/shutdown-window fail-loud** — NOTE (plan Task 15; spec §1/§3). Null-pool access
  (`Constructing`/`ShutDown`) is now FAIL-LOUD (`INVALID_STATE`), including the `Probe` class. A generic
  all-disks sweep racing table/server shutdown now sees a throw from the CA disk rather than a silent empty —
  intended (fail-loud > silent-skip; the old T8a null-pool wedge is structurally gone), but watch for
  benign-but-noisy shutdown-window throws in sweeps.
- (c) **GC `start()` partial-start desync guard (pre-existing)** — DEFERRED (T11 review, M4). `gcStart`'s
  re-enter of the scheduler `start()` has no guard against a partial-start desync (a worker/heartbeat pair
  left half-started, leaving the started/stopped flag inconsistent). Pre-existing, out of this round's scope;
  carried for a future GC-scheduler hardening pass.
- (d) **`RefWriter` DeathTest fork-under-load flake** — WATCHED (fix1 review, `1fe585ea078`). A `RefWriter*`
  `EXPECT_DEATH` test's `fork()` failed once (~1 ms) under full parallel gate load; 3/3 green isolated and on
  clean re-run. Class = fork-under-load, not a product red. Watch for recurrence; if it recurs, serialize the
  CAS DeathTests or lower gate parallelism around them.

## Write-path optimization candidates after stage 1 (2026-07-24) {#writepath-candidates-post-stage1}

Context: stage 1 (parallel intra-part blob upload) took the wide 10M×30col×500part CA-S3 `INSERT` from
58.41 s to 30.26 s (3.0× → 1.59× vs plain S3); the workload is still ~87% network-bound. Reports:
`docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md` (baseline),
`docs/superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect.md` (stage-1 effect). The residual
splits between the serial cross-part commit (stage 2's target, program point 7 — active, NOT a backlog
item) and the items below. STANDING USER VETO: the `HEAD`-before-`PUT` dedup gate (~12% of wall,
268.8 `HEAD`/part) and any change to the durable-op protocol are NOT candidates.

- (1) **Enable S3-native staging on the wide-insert profile and measure** — the feature exists
  (opt-in, write-once conditional server-side copy; validated e2e). Local staging then upload moves
  every blob's bytes twice; native staging may cut wall on S3 backends. Zero new code: flip the
  setting in an s41 variant leg and compare. Status: MEASURE.
- (2) **S3 client concurrency/connection tuning for the upload pool** — with 16-33 threads now
  issuing PUTs concurrently, client-side limits (connections, per-request concurrency) may cap
  overlap. Config-level experiment on s41. Status: MEASURE.
- (3) **Inline-placement threshold tuning** — small part files inline into the manifest
  (`CaInlinePlacement` machinery). The wide profile pays ~239 `PUT`/part (~8 objects/column);
  raising the inline threshold could fold the small tail (marks, minor streams) into the manifest.
  First verify the threshold is a setting (not a pinned format constant), then measure PUT-count and
  wall deltas on s41. Status: INVESTIGATE THEN MEASURE.
- (4) **Repoint waste on part removal** — known class (`project_part_removal_repoint_waste`):
  repoints against `delete_tmp_*` refs ≈ 22% of the writer `PUT` class. Eliminating them changes
  WHICH ledger ops are issued — protocol-adjacent, needs an explicit user decision with a risk
  analysis before any work. Status: DECISION NEEDED (present risk analysis to user).
- (5) **Unconditional manifest `GET` on promote** — part of the 108.7 `GET`/part during insert;
  separate long-standing item. Verification semantics of the write path → under the spirit of the
  protocol veto; do not touch without an explicit user go-ahead. Status: DECISION NEEDED (present
  risk analysis to user).

## Stage 2 (concurrent commitPart) — research notes; POSTPONED by user decision (2026-07-24) {#stage2-concurrent-commitpart-postponed}

USER DECISION: postponed — "слишком сильное / малопредсказуемое влияние на upstream / generic code". Recorded
here so the research is not lost; revisit only with an explicit user go-ahead.

Motivation (measured): after stage 1 the wide CA-S3 `INSERT` residual is 1.59× vs plain S3, dominated by the
serial cross-part commit (`ReplicatedMergeTreeSink::finishDelayed` iterates partitions one at a time; ref-ledger
batch size = exactly 1.0, so per-part manifest/ledger round-trips never batch). Stage 2 = bounded concurrent
dispatch of the per-partition commit; the CAS ledger then batches emergently and blobs multiplex on the stage-1
pool.

Agreed scoping (before postponement): (a) start with `ReplicatedMergeTreeSink` ONLY (the measured path);
(b) then re-run s41 with a non-replicated leg; (c) then the `MergeTreeSink` counterpart as a separate follow-up.

Path anatomy + hazard inventory (from code reading, 2026-07-24):
- Replicated loop body per partition: `finalize` → dedup hashes/block-ids → `commitPart` (Keeper block-number
  alloc → `renameParts` disk txn [the whole CAS write path lives here] → Keeper multi ~`:995-1011` → rollback
  machinery) → dedup-conflict retry loop (`deduplicateBlock` filters the block, then `writeNewTempPart`
  RE-SERIALIZES AND RE-UPLOADS the part, then retries commit) → `resolveQuorum` WAITS inside the iteration →
  `PartLog::addNewPart`.
- Concurrency hazards found: `deduplication_async_inserts_cache_version = 0` reset per iteration is a SHARED
  member (`ReplicatedMergeTreeSink.cpp:455`) — race under fan-out, must become per-task; shared Keeper session
  via `ZooKeeperWithFaultInjection` (raw client is thread-safe; the fault-injection wrapper needs verification);
  shared caches `deduplication_hashes_cache` / `async_block_ids_cache` `triggerCacheUpdate` from multiple
  threads needs verification; quorum ordering semantics change (today partition N+1 does not commit until N's
  quorum resolves) — recommendation was to force serial when quorum is enabled; a FULL shared-state inventory
  of `commitPart` (storage counters, rollback checkpoints) was identified as the main design work and was NOT
  completed.
- Plain `MergeTreeSink::finishDelayedChunk`: simpler loop (finalize → `deduplication_log->addPart` →
  `renameTempPartAndAdd` → PartLog); hazards: non-replicated dedup-log append concurrency, too-many-parts
  delays. Unmeasured (s41 is Replicated).
- Patch shape (approach 1 of 3, recommended at the time): private `processDelayedPartition(partition)` +
  bounded `ThreadPoolCallbackRunnerLocal` fan-out + setting `max_concurrent_part_commits_per_insert`
  DEFAULT 1 (feature dormant = today's serial behavior; minimal fork-rebase risk), all-drain + first-error,
  per-task `ProfileEventsScope`, B90 capture discipline. Estimated diff ~100-150 lines. Rejected alternatives:
  commit-only fan-out with caller-side retry queue (async state machine, NOT compact); window-2 pipeline
  (complexity without the win).
- Expected effect calibration: even a perfect stage 2 does not reach 1.0× — ~12% of wall is the vetoed
  `HEAD`-before-`PUT` dedup cost; realistic target ~1.2×.

## GC throughput collapse under a mass-DROP burst — RCA landed, three separable defects {#gc-throughput-collapse-2026-07-25}

Root cause (CA-s3 stateless lane, mass table CREATE/DROP churn): rounds are serialized and drain all
available work with no regular-round budget, so once DROP arrivals between round completions exceed
one round's service rate, round time diverges (a queue-stability crossing, self-inflicted, not
external — round wall time went 20s to 1716s over 6 rounds as candidates went 188 to 20,046). Three
separable defects, all still open:
- **The meta pool's "bounded" queue has zero depth**, so `scheduleOrThrowOnError`'s unbounded block
  serializes the fold thread's own LIST/GET work behind condemn-marker PUT latency
  (`CasGc.cpp:194,226`). A deeper queue buys overlap, not a lower floor — needs measurement before
  tuning, since the endpoint can already be saturated.
- **Completed namespaces keep permanent tombstones under the globally-enumerated ref prefix**, so fold
  cost grows linearly in historically-unique namespace count forever. Needs a protocol split (spec
  first, not a patch) separating completed-namespace tombstones from the enumerated prefix.
- **`system.remote_data_paths` has no `disk_name` pushdown** — upstream code, tracked separately at
  [`{#remote-data-paths-no-pushdown}`](#remote-data-paths-no-pushdown); amplified but did not cause
  this collapse.

**Gated on a measurement rig that does not exist yet**: the GC has no timing instrumentation at all
(no per-phase wall time, no per-phase S3 request count) — every number above was reconstructed from
log timestamps. Before choosing a production fix: (1) ship per-round per-phase introspection (wall
time + request count by verb, surfaced via ProfileEvents/`system.cas_log`); (2) build a reproduction
rig driven by namespace churn (not data volume — the ca-soak driver is the wrong instrument); (3)
measure the queue-stability boundary and whether it's self-limiting. A naive fold budget is unsafe by
construction (an omitted `+1` edge must suppress deletion, never permit it), so any bounded-round
design needs durable progress state, hence a spec, not a patch.

## fsck-vs-GC blob-retention leak — safety proven, root cause of the arrival still open {#fsck-gc-indegree-disagreement-2026-07-25}

Root-caused 2026-07-25: a live soak showed 56 blobs (104,755 bytes) that fsck's reachability walk
finds unreferenced while GC's in-degree view assigns them in-degree 1 and never nominates them —
flat across 1062+ rounds. Traced to an unmatched-minus-one: each blob has exactly one residual
source edge whose owning manifest is gone, so the `-1` that should have cancelled the `+1` was a
silent no-op (set-presence merge on an absent key). **Safety is proven** — this is a debris/retention
class, not data loss (the edge-set model cannot over-delete; see the already-closed
unmatched-removal-fold finding) — and the observability ask (count unmatched-remove no-ops) is
already live as `CasGcUnmatchedRemoveDeltas`. Only `ca-gc-rebuild` (full traversal) clears existing
debris; the incremental round cannot.

**Still open: why the `-1` arrives with no matching `+1` to cancel.** Event-log forensics traced one
occurrence to four `tmp-fetch_*` refs published and dropped inside a 43 ms window, with the `+1`
folding ~3 minutes after the drop — leading, unverified hypothesis is a fold-order inversion (the
removal record applied before the add record). Needs a targeted reproduction (concurrent fetch
publish + immediate drop) in the fetch-handoff area, not more log archaeology. The ca-soak harness's
`(M-F debris, B140)` mislabel for this population is tracked at
[`{#fsck-large-pool-fixed}`](#fsck-large-pool-fixed).

## LIST-as-journal data-loss class — RESOLVED by the v9 redesign {#list-as-journal-dataloss-2026-07-25}

The 2026-07-25 finding (GC discovering ref-log transactions via a paginated `LIST` with no
contiguity proof, so an omitted page could silently authorize a live blob's deletion) is
structurally closed by the v9 ref-table redesign: dense per-life ref ids, an in-band epoch seal,
and a `_ckpt` recovery frontier make the defect class unrepresentable rather than merely detected.
Recovery no longer reads listings at all (recovery-from-authority). Full incident facts, legal-lie
classification, and the passport design for future LIST-trust optimizations are the settled record
at [`2026-08-03-list-trust-verdict.md`](2026-08-03-list-trust-verdict.md) — do not re-litigate here.
The one still-open, separately-tracked follow-up is the LIST-trust optimization plan at
[`{#gc-frontier-one-list}`](#gc-frontier-one-list).

## The adopted fold seal referenced a PRUNED generation's run (observed 2026-07-25, same stand) {#adopted-seal-pruned-run-2026-07-25}

Once observed: an adopted fold seal's `blob_target_runs` named a generation's run object that
`pruneSupersededGenerations` had already reclaimed, despite `CasGc.cpp:629-645`'s explicit
"retention must never reclaim these" guard over the parent seal's referenced generations. While it
lasts, `zeroInDegree` sees zero candidates from the absent run and the round silently reclaims
nothing (no throw on 404 per [[feedback_ca_gc_never_throw_on_404]]). NOT ESTABLISHED: whether the
prune raced the adopt, or the parent-seal capture missed the carry. Needs a targeted test, not
log archaeology.

## Operator recovery: mounting a pool whose owner uuid differs — not decided, not started {#operator-uuid-recovery}

A server whose local uuid file was regenerated (wiped data dir, a pod recreated without a persistent
volume) cannot mount its own pool. `CasServerRoot.cpp:120-131`'s refusal already names three manual
recoveries (restore the uuid file, configure a fresh `server_root_id`, or delete the owner object by
hand after verifying no server uses the root); a supported command would automate the third. **Open
design choice**: overwrite the owner uuid with a new one (works, but permanently locks the original
server out, and must cover both the owner and mount objects to keep the epoch-1 re-mint guard armed),
or adopt the pool's existing owner uuid and mount as it (`Pool::openForDecommission`,
`CasPool.cpp:720-776`, already does exactly this — the reading that looks strictly better). Not the
read-only-mount task, which is separate and unimplemented.

## Part B pre-merge review — RESOLVED except one residual window {#partb-review-findings}

The 2026-07-25 codex review of the publish-confirm protocol (23-file diff) found two blockers and
two majors; fixed in `8e6fe6ef0af` (gate 1373/1373, five new tests each seen red first, integration
11/11). Blocker 2 (uncertain `precommitAdd` losing its cleanup owner), major 3 (promote reported as
fallback after a real commit), and major 4 (move assignment discarding a terminal duty) are closed.
Blocker 1 ("a completed recovery can expose a stale row and authorize `Yes`") was downgraded, not
fixed: the user correctly identified it inherits the mount's own trust rather than introducing a new
one — it is the same [`{#list-as-journal-dataloss-2026-07-25}`](#list-as-journal-dataloss-2026-07-25)
finding through the confirm lens, and that finding is itself now resolved by the v9 redesign. **One
residual window remains, flagged not fixed:** `eraseView` still runs after the durable commit and can
throw, and `ContentAddressedTransaction::publishStaging`'s `out_slot` is assigned only after
`promoteBuild` returns — closing it means extending the no-throw-after-commit discipline one frame
outward.

## fsck large-pool reporting: three residuals left after the 2026-07-26 fix {#fsck-large-pool-fixed}

Task #13 fixed `corrupted_runs` visibility/fatality, the inverted timeout budgets that made
`--partial` unreachable, and the fabricated-clean-on-partial hazard (all landed, tested). Three
residuals remain open:
- **The `M-F debris, B140` mislabel** (`checker.py`/`run.py`/`plot.py` docstrings) is still printed;
  the product classifies these as `AwaitingGc`, not the old B140 rationale. Docstring cleanup only.
- **`FsckTimeout` still substitutes fabricated `{"dangling": 0, ...}` zeros** on the remaining
  timeout path (`{#fsck-fabricated-clean-on-timeout}`). Not a live defect (every consumer is guarded
  by `not _detail_fsck_skipped`), but a landmine — fixing it needs auditing every downstream
  `f.get(...)`, deliberately not bolted onto the task #13 fix.
- **The GC-checkpoint entry-gate fsck still does not finish** on a pool as small as 5.5 GB within
  its 180 s budget — task #13 made the timeout degrade honestly (a logged skip, not a fake OK), but
  the gate still does not run. Options not yet chosen: scale the budget with pool size, use
  `--partial` deliberately as a lower bound, or make fsck itself cheaper.

## Probe A / LIST-hole investigation — superseded, settled record moved {#probe-a-direction-evidence}

The probe-A ref-prefix-enumeration-hole investigation (task #12/#18/#20, the "caught live"/"proven
by measurement" firings, the audit-detail fix) is now fully captured by the settled reference doc
[`2026-08-03-list-trust-verdict.md`](2026-08-03-list-trust-verdict.md), which cites these exact
findings as its source material. Probe A itself was deleted from the code by the v9 redesign
(recovery-from-authority no longer reads listings at all — see
[`{#list-as-journal-dataloss-2026-07-25}`](#list-as-journal-dataloss-2026-07-25)). The one loose
end this investigation surfaced — "path 2": a `-1` arriving before its `+1` produces an unmatched
remove that the reducer correctly drops as a no-op — is the same edge-set mechanism the
already-resolved unmatched-removal-fold finding covers (pinned by
`CasBlobInDegree.*.UnmatchedRemovalIsAPerKeyNoOpAndSparesSiblingEdges`).

## GC round duration — measured, both fix levers already tracked {#gc-round-duration-answered}

Root cause measured: GC round duration is 100% serial round-trip latency (~0.5 ms/request, no CPU or
lock term), not repeated or unnecessary work — a 30-minute round is 3.42M serial round trips (intake:
1 GET/log + 1 HEAD+GET/manifest edge; deletes: 1 conditional `deleteExact`/object, no bulk-delete path
because it's token-conditional and safety-critical). Both fix levers this measurement identified are
already separately tracked: parallel fetching/deleting (`{#gc-delete-concurrency-serial}` and the
sibling intake lever) and dropping redundant manifest-body re-reads (folded into
`{#gc-frontier-one-list}`). Operational finding from the same investigation: `--scale full` S42 needs
~400 GB free disk and does not fit this host; use `--scale ci` with headroom checked first.

## S42 at `ci` scale: the OOM machinery HELD; the run failed on the environment {#s42-ci-verdict}

Status line: `S42 DONE: status=FAIL (26/28 verdicts pass)`. Both failures are ENVIRONMENTAL, and the
distinction is the whole result.

### The safety signals — all clean {#s42-safety}

```
QueryMemoryLimitExceeded     2184   <- the injected faults DID fire; the run is NOT vacuous
CasRefApplyPoisoned             0   <- THE critical invariant: no writer's view is untrustworthy
CasRefAppendWedged              2
CasRefAppendUnwedged            2   <- both wedges RESOLVED; fail-closed worked end to end
CasRefAppendDefiniteFailure     0
CasGcUnmatchedRemoveDeltas      0   <- no retention-leak signal
acked blocks               11,960
```

**On the question actually asked — do we break under memory exhaustion? — the answer is no.** 2,184
allocation faults landed, no cache was poisoned, both wedged ref lanes recovered, and every acked block
survived.

### The two failed verdicts, and why they are the environment {#s42-env}

**1. "statements failed only with the injected allocation error" — 23,561 other failures.** Every sampled
one is `Code: 210 ... CAS write could not be committed (stageManifest: part-manifest PUT is UNCERTAIN
(retry budget exhausted))`. That is a request-timeout cascade, not a memory error.

**2. "GC no Failed rounds" — 2.** Both are `Code: 499 ... Timeout ... (S3_ERROR)` on objects of **268 bytes**
and **4,606 bytes**. A 268-byte GET timing out is a store that is not answering, not a product that cannot
allocate.

### Why the environment was compromised, and why that was predictable {#s42-env-cause}

This run started while the host was still recovering from the `--scale full` attempt
({#s42-full-scale-too-big}): load average was 22–48 at launch, and RustFS shares the machine with two
ClickHouse servers and the workload. Under that, small-object requests time out and the retry budget
empties.

**The card's gate did exactly its job.** It is built to fail when non-injected errors appear, precisely so
attribution is never muddied — and here it caught a polluted experiment rather than a defect. That is the
gate working, not the gate being wrong.

### Verdict, stated the way it should be recorded {#s42-verdict-wording}

- **On memory-exhaustion safety: PASS on every signal**, with the anti-vacuity gate satisfied (2,184 faults).
- **On the card's own strict criterion: FAIL**, because the environment injected 23,561 failures of its own.
- **Therefore the run is INCONCLUSIVE as a certification** and must be repeated on a quiet host before S42
  can be called green. It is NOT evidence of a defect.


## The fsck exit set and SQL row still have no test that can fail for them {#fsck-untestable-render-surfaces}

Residue of Task 1c fix round 2, named by the implementer rather than found later. The hard-finding
rule now EXECUTES — `FsckReport::clean` is computed from `kFsckHardFindings`, and a `static_assert`
on that list's deduced size trips in all three surfaces' translation units — so the rule no longer
depends on a reader remembering it. What remains is narrower and is the only route left to a fifth
recurrence: **an author who reads the assert as an arithmetic complaint, bumps the count, and does not
visit the three surfaces.** The summary line has a real test; the nonzero-exit set and the SQL row do
not, because `contentAddressedFsckColumns` and `appendContentAddressedFsckRow` have internal linkage
in an anonymous namespace, and `programs/disks` is not linked into `unit_tests_dbms`.

Closing it means giving those functions external linkage plus a header — a structural change to
`src/Interpreters/InterpreterSystemQuery.cpp`, a shared non-CAS file. **CONSULT ITEM, not a task:**
per the standing rule that shared/upstream surfaces are not edited without consultation, this needs a
decision before anyone implements it. The cheap alternative worth weighing first is whether the assert's
message can be made harder to satisfy without visiting the surfaces at all.

Also recorded: `05020_content_addressed_fsck.reference` pins the row via `TSVWithNames`, so it is a
ONE-DIRECTIONAL fence — it fails on a column added without updating the reference, not on a `clean()`
term added without a column. Only the `static_assert` covers that direction.

## The fsck exit-code rule is restated in prose the fence cannot reach {#fsck-rule-restated-in-unfenceable-prose}

Largest residue of Task 1c fix round 3, named by the implementer. The rule now EXECUTES in code
(`FsckReport::clean` computed from `kFsckHardFindings`, `static_assert` tripping in three TUs), but
the same rule is also RESTATED in prose that no build can check — `docs/superpowers/cas/08-testing-and-soak.md`
and two harness files. **Three of those restatements were found wrong in one round**, one of them
inside an operator-facing warning string, and one was wrong about three separate terms.

Nothing mechanical will catch a fourth. The real fix is structural and is a documentation decision, not
a code task: **the doc should point at the code rather than restate the exit set.** A reader who needs
to know which findings exit nonzero should be sent to `kFsckHardFindings` and to
`CommandFsck::executeImpl`, not handed a list that drifts. The same applies to the harness's comments.

Related: `{#fsck-untestable-render-surfaces}` is the other half — the exit set and SQL row have no
failable test. Together they bound what the fence does and does not reach; do not let a future round
re-derive either from scratch.

## A loose mountpoint object under `_files/` is classified as a corrupt namespace file {#loose-mountpoint-object-as-corrupt-namespace-file}

`Layout::mountpointObjectKey` does not enforce the `_files` reservation its own doc comment
claims, so a loose object at `roots/<srid>/_files/x` satisfies `parseNamespaceFileKey`'s necessary
condition and gets treated as ours — `ca-decommission` refuses fail-close and `ca-fsck` posts a
hard `lifeless_keys` finding against a key that is not damage. Direction is safe (refuse/report,
never delete) so not urgent, but a hard finding against an undamaged key trains an operator to
disbelieve hard findings. **Open decision**: enforce the reservation in `mountpointObjectKey`
(makes the existing doc comment true) or narrow the classifier.

## Stateless drain tests still owe their assertion restoration (Task 7b) {#stateless-pending-double-count}

`04290_content_addressed_no_leftovers.sh`/`04295_content_addressed_mutation_no_leftovers.sh`'s
`PENDING` double-counting (`pending_condemned` already totals candidates+retired) is fixed — both
tests now read `pending_condemned` alone (`8e9b06c2a81`, 2026-07-30). Task 7b still owes restoring
their assertions (both are currently registered known-red).

## The `!attempt_armed` arm's non-deletion is unpinned, by a judgement I endorsed {#attempt-armed-arm-unpinned}

Task 3 fix round 1 removed a `cleanupOrphanedBirthCkptBestEffort` call from `commitRefChunk`'s
`!attempt_armed` arm, because that arm makes no lane transition and so is not covered by the delete's
safety argument — and in one shape the append that advanced applied state can BE the birth whose `_ckpt`
would be deleted, against an object with no repair path.

**No test pins the non-deletion, and that is deliberate.** The implementer looked for a reachable path
and found none: `leader_active` serialises each table to one flush leader, and nothing mutates
`rt->state` between `commitRefChunk`'s snapshot and its arming check on that same thread. A test would
therefore either not enter the branch — testing nothing — or need a new test-only hook poking
`rt->lane_state` / `append_attempt` / `state` directly to force the mismatch, bypassing the machinery.
It declined to add such a hook unreviewed inside a fix round, and I endorse that: a hook whose purpose is
to construct states the real code cannot reach is a way to make impossible states testable, which is a
design decision, not test hygiene.

**So the residual risk is re-introduction, not present behaviour.** If someone adds the call back to that
arm, nothing fails. If that is worth closing, the honest options are (a) build the hook as its own
reviewed change, with the arm's neighbours getting the same treatment since they share the posture, or
(b) accept it as defensive code and leave this entry as the record. Do not close it by re-adding the
call and calling the arm safe.

## Every TLA runner shares a STATIC metadir, so two overlapping runs corrupt each other {#tla-runner-static-metadir}

Observed 2026-07-30 during Task 10c, as a FALSE RED that cost a diagnosis. `CaB140DangleMerge`'s
positive config `m_merged` came back `error` with TLC reporting a missing file inside its own state
pool:

```
Error: when reading the disk (StatePoolReader.run):
  ../../../tmp/tlc-meta-CaB140DangleMerge-m_merged/.../372 (No such file or directory)
```

Every runner in `docs/superpowers/models/` passes `-metadir` a path derived only from the model and
config names — no run identity — so two invocations of the same config share one directory and the
second one's cleanup deletes state the first is still reading. Re-run on a clean metadir, `m_merged`
completes GREEN in 9s over 20692441 states / 5326000 distinct, so the model was never at fault.

**Why this is worth fixing rather than remembering:** a red that is really an infrastructure collision
is the most expensive kind of red, because the honest response to it is to stop and investigate the
model — which is exactly what happened, and what the runner's own instruction to stop on
`SOME EXPECTATIONS UNMET` correctly forced. It will recur whenever two runners, or a runner and a
manual TLC invocation, touch one config concurrently.

**Fix:** give the metadir a per-invocation component (the runner already computes a unique `LOG_ROOT`
per run — derive the metadir from the same value). It is a one-line change per runner, but it is a
sweep across all of them plus a convention note in `models/README.md`, so it wants its own task rather
than riding along with a model change.

## Empty-set survey: one violation I reproduced but could not explain, and one clean refusal {#empty-set-survey-residues}

From Task 10f part 2 (the empty-entity-set survey, `docs/superpowers/models/2026-07-30-empty-set-survey.md`
on `cas-gc-rebuild-t10`). Of ~26 models: about twelve green, **eleven have no entity-set constant at all**,
one TLC error, one violation.

**The clean refusal — `CaBuildRootPrecommit` with `Trees = {}`:** `Error: Assumption line 108 ... is false`.
The spec explicitly ASSUMES non-emptiness. That is the ideal outcome: the model says out loud that it does
not model this case, instead of quietly answering as though it did.

**The violation I could not explain — `CaRefWriterCleanupCore` with `Builds = {}`.** Temporal property
`StalePrecommitEventuallyGone == StaleExists ~> NoStale` reported violated, counter-example
`Init → Fence → RemoveNamespace → Stuttering`. **Reproduced independently by me**, twice, so it is not the
static-metadir false red.

But the analysis says it cannot be a genuine violation: the dumped counter-example shows every
build-indexed variable empty (`<< >>`) in **every** state, so `StaleExists` (`\E b \in Builds`) is FALSE
throughout and `NoStale` (`\A b \in Builds`) is TRUE throughout — and `FALSE ~> TRUE` is trivially true.
**I did not identify the mechanism**, and I am recording that rather than guessing one; the likeliest place
to look is how `Spec`'s fairness composes when every fair action is permanently disabled.

**Why this matters more than the one model.** Whoever adds empty-set configs as a convention (Task 10f
part 1) will meet artifacts of exactly this shape, and **an artifact indistinguishable from a bug is worse
than an untested case** — it trains the reader to dismiss reds. So part 1 owes a rule for telling the two
apart, and `CaBuildRootPrecommit`'s explicit `ASSUME` is the model to copy: a spec that does not cover the
empty case should REFUSE it, not answer.

**And the deeper result of the survey is not the reds at all: eleven models have no entity set**, including
`CaRefCatalogCore`, `CaRefLaneCore` and `CaRefTableSnapshotLogCore`. They model a single instance, so they
cannot express "zero namespaces" because they cannot express "N namespaces" either. That is the real answer
to "did we model this?" — for R11 the vacuous-empty class was not merely untested, it was **inexpressible**,
and the gate model part 1 must build will need a namespace SET that none of the existing catalog or ref
models has.

## RULE: a reachable and handled state must not be `chassert`ed {#rule-no-chassert-over-handled-branch}

From a sanitizer run on 2026-07-30 that failed `CasAnomalyPolicy.NonReadyAtNewIdAllocationFaultsAndFailsClosed`
three times for two different reasons. Recorded as a rule rather than a bug because it is the second face of
a confusion we have already been bitten by.

`CasRefLedger.cpp` carried `chassert(lane_state == RefLaneState::Ready)` four lines above
`if (lane_state != RefLaneState::Ready) { fault the lane; anomaly policy; CORRUPTED_DATA }`, whose own
comment says that any other state "is an internal lifecycle violation … make that contradiction an explicit
`Faulted` state and route it through the anomaly policy". So the handling IS the design — and the assert
above it means that on debug and sanitizer builds **the entire fail-close path is dead code**, while the
process aborts on precisely the contradiction the path exists to contain. On release the assert compiles out
and the path works, so the two build families disagree about whether the code exists.

**The rule: if the code below handles the state, do not assert it above. The handling is the assertion.**

**This is the INVERSE of the rule we already had** — `{#review-blindspots}`'s "a `chassert` is not a
release fail-close" — and both come from the same confusion about what a `chassert` is for. Together:
- a `chassert` cannot be the fail-close, because release does not have it;
- and a `chassert` must not sit over a fail-close, because sanitizers then do not have the fail-close.
A state is either impossible (assert it, handle nothing) or possible (handle it, assert nothing). There is
no third option, and picking both is how a branch comes to exist in only half the builds.

**The same run also caught a real race, and its scope was wider than reported.** The test's event sink
pushed into a bare `std::vector<CasEvent>` while being called from background threads. The report named two
sites; there are three, and **all three carry the same comment about outliving "the background syncer's
emits"** — so every one of them knew the sink runs on other threads and none synchronised. When a hazard
comment is copied along with the code, the copies inherit the hazard and not the fix.

## RULE: changing a returned element type silently re-binds every `auto & [a, b]` at its call sites {#rule-structured-binding-silent-rebind}

Found 2026-07-30 during Task 4-C, and it is the sharpest defect shape we have seen: introduced by a
change, located in code the change did not touch, and invisible in the diff at the site where it went
wrong.

`gtest_cas_gc_shard_incarnation.cpp`'s `DiscoveryEqualsPresentShards` iterated
`for (const auto & [ns, shard] : universe)`. Task 4-C changed `discoverUniverse`'s element type to
`NamespaceLifeId` — a two-field struct, not a pair. **That still compiles**, because C++17 destructuring
works on any aggregate whose public member count matches, so `shard` silently became `.incarnation` and
the comparison `shard == 0` could never be true against a nonzero incarnation. No compile error, no
warning, and nothing in the changed files to review.

**The rule: when you change what a function RETURNS — element type, member count, member order — grep
every call site for destructuring (`auto & [`, `auto [`) and read each one.** A rename or a signature
change gets a compiler error; an aggregate reshape does not. Member ORDER matters too: swapping two
same-typed members re-binds every destructuring silently and changes nothing else.

**And the corollary for reviewers:** a diff that changes a return type is a diff whose blast radius is
NOT in the diff. Ask what destructures it.

Related in kind: `{#rule-no-chassert-over-handled-branch}` — both are cases where the compiler's silence
is mistaken for the code's agreement.

## `CasGcStopStart` needs 69s under a 60s per-suite gate timeout {#casgcstopstart-exceeds-suite-timeout}

Observed 2026-07-30 during Task 4-C's gate run. The suite fails in the per-suite gate script and **passes
standalone in 69s**. The script's per-suite timeout is 60s, so this is not a flake — it is a suite that
**cannot** pass in that harness, and will report red on every run until one of the two numbers moves.

Worth separating from the flake bucket precisely because the failure is deterministic: a test excluded as
"timing-related" gets re-triaged every few weeks, whereas a test that provably exceeds a fixed budget has
one decision attached to it — raise the budget for that suite, split the suite, or make it faster.

Also note what it means for any tally taken from that harness: a suite killed at 60s contributes a
failure that says nothing about the code, and a reader comparing tallies across runs will see it move with
machine load rather than with the tree.

## The gate suite list silently omits suites — THIRD recurrence, because both prior fixes fixed the data {#gate-suite-list-omits-third-recurrence}

Found 2026-07-30: `tmp/cas_suites.txt`'s generation matches `Cas*:CA*`, which cannot match
`CaWiring`/`CaLifecycle` (lowercase `Ca` matches neither `Cas` nor `CA`) or several `RefWriter*`
suites — 20 suites, 17 CAS-relevant, ran by no gate pass in an entire session, so every gate figure
that session was computed over an incomplete population. THIRD recurrence of this shape; the prior
two fixes each added the missing suites to the list (a data fix that regenerates away, since the
list is per-session). **Open fix**: fix the generator/pattern, then add a loud check — diff the
generated list against `--gtest_list_tests` and fail on any unclaimed suite.

## Refactoring candidates, derived from what actually broke {#refactor-candidates-from-defects}

Ranked by value-per-risk, each backed by real defects it would have prevented.

1. **DONE, differently than proposed.** "Make catalog-life absence expressible in the type
   (`resolveLifeOrSentinel` → `std::optional`)" was the top item here; the current API already does
   this under a different name — `CasRefCatalog::lifeIfCataloged` returns
   `std::optional<NamespaceLifeId>`.
2. **One life resolution per round, threaded — not re-derived.** Five mechanisms still answer the
   same question across ~80 call sites (`resolveNamespaceLife`, `discoverUniverse`,
   `stageATransition`, `fromCatalogEntry`, plus the now-optional lookup). The fold has
   `FoldResult::live_incarnation` to consult instead of re-resolving; fsck and decommission still
   re-resolve per call.
3. **The destructive gate collapses per-namespace facts into a pool-wide boolean.**
   `suppress_destructive` is a single scalar OR over every namespace's anomalies/holds/frontier
   state, so one un-cataloged namespace stalls reclamation for the whole pool. Wants to be
   per-namespace.
4. **`Gc/CasGc.cpp` and `Pool/CasRefLedger.cpp` are ~18% of the subsystem by line count** — not an
   aesthetic complaint, real defects have hidden in both files' size. Extraction needs equivalence
   fences written BEFORE the move, not after; not during open Criticals.
5. **The fixture/production divergence should be one named seam, not a habit.** Raw test helpers
   write at the sentinel and bypass birth in ways production code never does; one documented helper
   instead of ad hoc divergence.
6. **Keep converting prose rules into executing checks.** Two conversions already held immediately
   (`FsckReport::clean` from a `static_assert`-guarded list; the gtest suite-list generator failing
   loud on any unclaimed suite) — every remaining "whenever X, also do Y" code comment is a candidate.

## A GC-level backstop for never-born `_ckpt` debris — and Task 3's closure of it was unsound {#ckpt-neverborn-gc-backstop}

Filed 2026-07-30 when Task 4-C removed all three `cleanupOrphanedBirthCkptBestEffort` call sites. **This
reopens `{#ckpt-failed-birth-debris}`, whose CLOSED marker from Task 3 must not be trusted** — the closure
shipped a delete whose safety argument contradicted its own trigger conditions.

**Why the delete had to go, not be guarded.** All three call sites sit inside the `CORRUPTED_DATA` path,
which by `putIfAbsentControlled`'s own doc means *a different object already occupies this transaction's
derived key*. So every trigger condition **proves the ref-log is non-empty at that key**, while the comment
justifying the delete rests on "reachable only while the namespace's ref-log has never durably held
anything". Contradictory, and contradictory on every branch that calls it — not in a corner case. A
successor that observed the seal and adopted the live incarnation could have its current `_ckpt` deleted, and
`_ckpt` has no repair path.

Binding the delete to a token captured at publish time does **not** fix it either: a successor that only
READ `_ckpt` leaves the token matching, so the delete still succeeds against a record that successor's
recovery already leaned on. And a partial guard standing beside a removed operation reads as a licence to
reinstate it, which is the shape that has already cost this campaign a "fix" that made a correct change look
like a regression.

**The trade, stated as a trade.** We exchanged *may delete a live successor's `_ckpt`, unrecoverable* for
*debris survives until a backstop exists, operator-visible*. The cost is real: permanent debris makes a
drained server root REFUSE decommission (`claimOwnerOrThrow` → `CORRUPTED_DATA`), which is exactly what the
original entry described. Accepted, because a fallback path must never take a destructive action — skip it
and surface the problem.

**What the backstop must do**, mirroring the existing REMOVED-namespace `_ckpt` backstop in `CasGc.cpp`:
reclaim a never-born namespace's `_ckpt` only after **independently re-verifying emptiness with a real
LIST**, never by inferring it from one attempt's own conflict. That independence is the entire difference
between the backstop and the thing that was removed.

## The uniform catalog-admission pin removed the suite's ability to test the un-admitted case {#uniform-pin-removed-testability}

Third distinct consequence of one mechanical sweep, found 2026-07-31. Recorded because the first two were
findings about individual tests and this one is a property of the sweep itself.

Task 4-C fixed 164 failing tests by adding `casAdmitEntry` inside `writeRefLogTxnRaw` — the single choke
point every raw-write fixture funnels through. Correct, and it collapsed 164 bespoke fixes into one. But it
also means **no raw-fixture helper can construct an un-cataloged namespace with content any more**, which is
exactly the shape the un-cataloged anomaly exists to detect. So the sweep quietly removed the suite's means
of testing what it was protecting, and no test of that shape existed until one was written deliberately.

The way back in: build the namespace through the **real writer path** and strip its catalog entry
afterwards, rather than trying to write content without an entry. That is what
`CasGcShardIncarnation.UncatalogedNamespaceWithRealObjectsLeavesTheRoundIncomplete` does.

**The other two consequences of the same pin, for the pattern:** it inverted the premise of the one test
whose subject was the ABSENCE of a catalog entry (a "never-born" birth-`_ckpt` test whose assertion then
demanded deletion under a `Live` entry), and it made `DropNamespaceErasesAllViews` — the one end-to-end
real-incarnation test — run entirely at the sentinel, which had to be undone by REMOVING its pin.

**The rule: after a uniform pin, ask what the pin makes unconstructible.** A sweep that adds a precondition
everywhere removes every test's ability to exercise its absence, and the tests that mattered most were the
ones whose subject was that absence. Grep the swept files for premises of absence — "never", "no entry",
"not yet", "un-cataloged" — before declaring the sweep complete.

**And a companion note on my own error in the same thread:** I claimed the property was already enforced by
existing regression tests because three tests sat next to the anomaly and mentioned it. They existed to
ROUTE AROUND it. Asserting an enforcement mechanism from a test's neighbourhood rather than its assertions is
the same class of mistake as every other false claim in this campaign — a statement about another location.

## `life_epoch` monotonicity holds PER SERVER ROOT — decommission must not break it {#life-epoch-monotone-per-server-root}

Recorded 2026-07-31 from Task 4c, which made a decreasing `_ckpt.life_epoch` contribution `CORRUPTED_DATA`
instead of letting `max` absorb it. That refusal rests on an argument with a stated limit, and the limit is
what this entry exists for.

`writer_epoch` is durable-monotone **per server root** — `allocateWriterEpoch` CAS-bumps
`<prefix>/gc/server-roots/<srid>/epoch` — and every live namespace is rooted at its own member's
`server_root_id`, so a creator and any actor that later reconciles it draw from **one** counter. That is what
makes "contributions only ever rise" true, and therefore what makes a decrease a fenced-out writer rather than
an ordinary race.

**If a namespace could ever be created by one server root and later have its `_ckpt` contributed to by
another, the argument fails**: the two counters are independent and unordered, so an honest contribution from
the second root could be numerically lower and would be refused as corruption. Nothing does that today.

**Pool-member decommission is where this would be introduced**, since moving or adopting a namespace across
roots is exactly the shape. Whoever owns that work must either keep a namespace's `_ckpt` contributions within
one root for its whole life, or replace the monotonicity argument with something that survives two counters —
and must not discover this by hitting the refusal. The limit is stated at `joinLifeEpoch` in the code as well,
so the constraint is visible where it is relied upon rather than only here.

## Test helpers: a third verbatim copy, while a shared home is already included {#test-helper-third-copy}

Six helpers — `ALWAYS_ADMITTED`, `generousDeadline`, `creatorFence`, `fixedTerminality`,
`findEntryForTest`, `admittedOnceThenFenced` — are duplicated verbatim from
`gtest_cas_ns_creation_lifecycle.cpp` into `gtest_cas_ref_ckpt_join.cpp`, which **already includes**
`cas_test_helpers.h` for `CountingBackend`, `namespaceBirthOp`, `publishCommittedOps` and
`seedPoolMetaForRestart`. The copy is honest about itself — the source file says the precedent exists — but
this is the third instance, and the shared header is one line away in the same translation unit.

The question worth deciding rather than drifting on: **what belongs in `cas_test_helpers.h` and what is
deliberately per-file?** A fence policy or a deadline is arguably scenario-specific and should differ
between files on purpose; if so, three identical copies mean the opposite is happening. Deferred out of the
Task 4c fix round as a convention question, not a defect.

## The pool-wide catalog is a write hot spot, measured on the CA-s3 lane {#ref-catalog-write-hotspot}

**Measured 2026-07-31, first full CA-s3 stateless lane since the catalog landed.** 62 test failures, of
which **52 fail on stderr alone** — the assertions pass — because writers log transient object-store
errors. The errors concentrate on one key:

- **137 of 250** S3 timeout lines name `content_addressed_s3/cas/ref_catalog`.
- **125** `hit a transient object-store error` lines, all code 499 (timeout), across **66 distinct
  namespaces**.

The failing tests are ordinary MergeTree ones — `00612_pk_in_tuple`, `00806_alter_update`,
`02265_column_ttl` — with no CAS content. They fail because every table creation in the pool writes the
**same** object, so a lane that creates thousands of tables serialises them all through one CAS loop.

**Why this is a design finding and not lane noise.** The catalog was adopted as the GC's authoritative
universe precisely because a pool `LIST` is unreliable. That argument is untouched. What the lane shows is
the cost side, which no test before it could expose: the object is pool-wide, every creation contends for
it, and the retry loop turns contention into timeouts rather than into waiting. A single hot object is also
exactly the shape the surrounding design avoids everywhere else — refs are per-namespace and `_ckpt` is
per-namespace, both so that unrelated tables never share a CAS target.

**What this does NOT say.** It is not evidence that the catalog is wrong, and the fix is not to go back to
`LIST`. The measurement is from a stateless lane, which creates tables at a rate no real deployment
approaches, and rustfs is not a production object store. Both push the same direction, so treat the
numbers as an upper bound on severity and a lower bound on the existence of the problem.

**Questions the next round must answer, in order:**
1. Does the write rate come from creation only, or does anything else write the catalog per operation? A
   pure read that births an entry would multiply it — the read-mints paths are already a known residual.
2. Is the retry loop's deadline shorter than the contention it now has to survive? A timeout under
   contention is a tuning answer; a timeout under no contention is a different bug.
3. Can the object be sharded, or entries batched, without giving up the single-object atomicity the GC
   universe argument depends on? The universe needs a consistent snapshot, not necessarily one key.

## `Mode::Native` tests write into the process working directory {#native-mode-cwd-litter}

**Found 2026-07-31 by a sweep for vacuous absence assertions; reported rather than fixed, because it is
hygiene beyond that sweep's mandate.**

`Mode::Native` ignores the storage root that `makeLocalObjectStorageForTest` makes unique per test, so
every Native-mode test that touches a key writes it into the **test process's working directory**.
Confirmed on disk: `build_debug/p/gen/tok`, `build_debug/p/rc/one`, `build_debug/k/dialect`,
`build_debug/pool/blobs/ab/abcdef…`, all re-stamped by a gate run, plus an older `p/gen/tok` at the
repository root from a run in a different directory.

Nothing collides today — `p/gen`, `p/rc`, `k/` and `pool/` are distinct — so this is not a live flake.
Two consequences that are worth naming:

- **One assertion cannot fail.** `NativeConditionalPutCountsOneAttemptAndCommitted` asserts
  `putIfAbsent("p/rc/one", …).outcome == PutOutcome::Done` against a file that survives from the previous
  run. It passes only because `LocalObjectStorage` does not enforce the conditional, so that `EXPECT_EQ`
  could not observe `PreconditionFailed` either way. The assertion is not the test's subject — the
  subject is the `ProfileEvents` delta — but a check that cannot fail should not read like one that can.
- **The litter is a shared namespace across every Native test in the binary**, so the absence of a
  collision today is luck, not design.

**Fix, when someone takes it:** anchor Native keys through `DB::Cas::tests::nativeKeyUnder`, which now
exists in `cas_test_helpers.h` for exactly this and states the hazard where a new Native-mode test will
look. Mechanical, one line per site.

## `[gc-mf-cleanup-durable-retry]` Manifest-cleanup GC phase needs durable retry, not a cap {#gc-mf-cleanup-durable-retry}

**Found by a 24h soak (`soak-t6b-report.md`) after `gc_round_manifest_cleanup_budget` landed as one of
T6b's per-round work-envelope caps; the setting was removed entirely rather than tuned.**

The post-CAS `manifest_deletes` phase (`Gc::runRegularRound`, `Gc/CasGc.cpp`) is a **one-shot pipeline**:
the ref-log intake cursor that discovers each owner-removed manifest's `-1` edge commits in the SAME
round's CAS that produces the `mf_cleanup` set, before the deletes run. A cap on this phase does not defer
the excess to a later round of the same pipeline — a cap-declined entry is never re-derived, because the
cursor that would re-derive it has already moved past the log that produced it. The only remaining
reclaimer is the (much slower) orphan-manifest sweep backstop, which drains roughly 100 objects per round
and cannot keep pace with a real burst.

Soak evidence: run-1 (cap=5000) left 112,518 entries skipped, of which 110,218 were still unreachable at
checkpoint time (checkpoint FAIL). Run-2 (cap disabled) fully drained all 223,714 entries in-round with
zero left unreachable (PASS). The user decision was that the knob must not exist at all — a cap here
converts a bounded burst into a permanent leak, which is worse than no cap.

**Fix direction, when someone takes it:** real bounding needs the edge-consumption point moved to AFTER
the delete succeeds (durable retry), not before it, so a cap-declined entry stays discoverable by the next
round's intake instead of being silently dropped. This is a natural fit for a future
`gc-frontier-one-list` focused session (post-Stage-B), since it touches the same intake/cursor machinery.

## `[soak-predown-textlog-scope]` `predown_dump.sh` only captures error-shaped `text_log` rows {#soak-predown-textlog-scope}

**Found by the T8 criterion-4 anomaly-arm injection** (Stage-B soak, `2026-08-03-stage-b-RESULTS.md`
`{#criterion-4-evidence}`): the GC round's own `INFORMATION`-level narration line — the exact text
explaining why destructive work was suppressed for that round, plus phase narration and hold-cause
detail generally — is not captured anywhere `predown_dump.sh` writes, because its `text_log` extract
(`text_log_error_shapes.tsv`) is scoped to error-shaped rows only. Once the cluster is torn down (or, as
here, simply reset for the next run), that narration is gone for good; the round's own structured
`system.content_addressed_garbage_collection_log` phase rows survived and carried the criterion, but the
human-readable confirmation did not.

**Fix direction:** `predown_dump.sh` should also capture `system.text_log` rows from the CAS loggers at
`Information` level, bounded by a time window and/or row cap (an unbounded dump risks turning the predown
step itself into the next `cas_log.tsv`-sized artifact). Not attempted here — recorded as a tooling gap
so the next investigation that needs this evidence doesn't rediscover the gap the hard way.

## `[damaged-object-diagnose-and-repair]` fsck must diagnose AND repair a damaged rebuildable object; the runbook must say how {#damaged-object-repair}

**Found by the T8 criterion-4 injection** (Stage-B soak; evidence pack
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-injection-evidence/`): a single namespace
checkpoint (`cas/ns/state/<life>/_ckpt`) was overwritten with garbage under a live writer. The GC fold
behaved exactly as designed — it detected the damage, classified the namespace as an anomaly/hold and
suppressed every irreversible family, round after round — but nothing in the product ever repaired the
object, and the live ref lane went to `CASRefNeedsRecovery` and stayed there for the remaining ~20
minutes of the run, including after the exact original bytes were restored. Byte-level damage to a
durable object is outside the trusted-store fault model this design assumes, so this is not a
correctness defect; it is an OPERABILITY hole: the system fails closed forever and hands the operator
no lever.

**What is missing, in priority order.**

1. **`ca-fsck` should diagnose the class precisely.** Today a damaged object surfaces as a suppressed
   GC round plus a counter; fsck's report has no row that says "namespace N's checkpoint is present but
   undecodable" (as distinct from absent, which is a legal cold-recovery state). Add the distinction:
   *present-and-undecodable* vs *absent* vs *decodable-but-inconsistent*, per affected object kind
   (`_ckpt`, fold seal, `gc/state`, catalog), naming the exact key.
2. **`ca-fsck --repair` (or an explicit sibling verb) should REBUILD what is rebuildable.** The
   checkpoint is a derived accelerator over the durable ref-log, so a damaged one is reconstructible by
   the same recovery walk the writer already implements (`recoverRefTableDetailed` / the recovery-epoch
   seal). The repair verb should: re-derive the object from its authoritative source, publish it by the
   ordinary CAS write path (no new object kinds, no protocol change), and refuse — loudly — for any
   object whose content is NOT derivable (a blob body, a committed ref-log record: those are the real
   data, and their loss is a restore-from-backup situation, not a repair).
3. **The lane must be able to leave `NeedsRecovery` once the source is sound again.** Our single
   observation says it did not, even after byte-identical restore. Whether that is a wedge, a
   remount-only exit, or an artifact of the injected shape is UNVERIFIED — determine it, and if the only
   exit is a remount, say so in the runbook and consider making recovery retry on its own.
4. **Runbook section: "a CAS object is damaged".** Operator-facing, in the numbered doc set, covering:
   how the condition ANNOUNCES itself (suppressed rounds naming the namespace, the fsck row from item 1,
   the `CASRefNeedsRecovery` counter); why there is no urgency (GC has already frozen everything
   irreversible — the pool is safe, it is just not reclaiming); the asymmetry an operator must know
   (an ABSENT checkpoint is a legal state that triggers cold recovery, a CORRUPT one is not — so the
   fallback of last resort is to DELETE the damaged derived object, never to hand-edit it); the repair
   sequence once item 2 exists; what NOT to do (`DROP POOL MEMBER` is for dead members, not damaged
   data; never hand-delete blob bodies or ref-log records; never "restore" bytes from an unofficial
   copy); and when the answer really is backup/restore because the damaged object is authoritative.

**Note on scope.** Items 1, 2 and 4 are operability work and need no protocol change. Item 3 may reveal
a real recovery-path defect; treat its outcome as its own item if so.

## [cas-join-set-truncate] `StorageJoin`/`StorageSet::truncate` throw retry-later, self-healing, on a CAS disk {#cas-join-set-truncate}

`StorageJoin::truncate` and `StorageSet::truncate` call `disk->removeRecursive(path)` then immediately
`disk->createDirectories(path)`. On a content-addressed disk `createDirectories` is a pure admission
no-op (`ContentAddressedTransaction::createDirectory` never touches the catalog), so the real re-mint
happens lazily on the first write after `TRUNCATE` returns — that write resolves the namespace through
`CasRefLedger::namespaceLife`.

**Verdict: TRANSIENT, not permanent.** A unit-level test
(`CASRefWriterNamespaceRemoval.FilesOnlyNamespaceTruncateThrowsRetryLaterUntilGcReclaimsThenRebirths` in
`src/Disks/tests/gtest_cas_ref_writer.cpp`) reproduces the exact sequence — birth a files-only
namespace life (the shape `StorageJoin`/`StorageSet` tables use, no MergeTree part ever published),
`dropNamespace` it (the `removeRecursive`-shaped call), then immediately call `namespaceLife` again on
the same name. It throws a typed `NETWORK_ERROR` ("CAS namespace … is Removing: creation waits for its
terminal fold and catalog removal to complete; retry later"), because the catalog row is still
`Removing` until a GC round actually deletes it. After draining GC (two rounds, same shape used
throughout this test file), the identical call mints a fresh incarnation and writes succeed normally —
self-healing, no operator action required.

Practically: `TRUNCATE` on a `StorageJoin`/`StorageSet` table backed by a CAS disk completes without
error (`removeRecursive`/`createDirectories` do not themselves touch `namespaceLife`), but the very next
write to that table (the next `INSERT`, or backup rewrite) throws a retry-later error until the
background GC round reclaims the just-removed row — a window bounded by GC round latency, not by
anything the client controls. A client without retry-on-`NETWORK_ERROR` will see the write it issues
right after `TRUNCATE` fail; retrying it (or simply waiting for the next GC round) succeeds.

**Before the `existsDirectory` fix** (the `DirShape::TableDir` cleanup-completeness probe), the same
`TRUNCATE` was silently a no-op on these engines: `existsDirectory` never reported the directory present
in the first place (it only answered "has at least one committed part", and these engines never publish
one), so `removeRecursive` was skipped entirely and the table kept its old contents. This is a change of
which wrong thing happens on `TRUNCATE`, not a newly introduced break: the old behavior silently ignored
the user's `TRUNCATE`; the new one executes it and imposes a bounded retry-later window on the following
write.

**Direction, not a fix here.** A real fix belongs in the CAS layer's rebirth semantics — either give
`namespaceLife` a fast, non-error path for "predecessor is provably terminal, just needs its row
folded" instead of forcing every caller through the GC-latency retry-later window, or have
`StorageJoin`/`StorageSet::truncate` itself wait for the removal to fully settle before returning
(mirroring `DROP TABLE ... SYNC`'s own synchronous-completion contract) rather than leaving the very next
write to discover the window. Out of scope for the fix-verify pass that found this; tracked here as a
usability rough edge, not a correctness defect.

## [disks-exit-code-upstream] `clickhouse-disks --query` non-interactive exit code — carve-out obligation {#disks-exit-code-upstream}

`DisksApp::main` now returns a failing command's error code as the process exit code for
non-interactive `--query` runs, so CI and cron can gate on `clickhouse-disks` at all. It **rides in the
CAS pull request for now** — pre-release, and the gating it enables is needed there — but it is a
behavior change to a shared tool for every user of it, so it must later be carved out into its own
upstream PR together with the integration-test fix it forces.

The record lives with the carve inventory, not here: `docs/superpowers/cas/upstream.md`, §G list plus
the G-item section below it (site, rationale, the two reviewer-facing details, the latent
`test_replicated_table_structure_alter` defect it exposed with its mechanism, and the blast-radius
conclusion). Listed as Workstream A1 in
`docs/superpowers/specs/2026-07-28-cas-merge-layout-preparation-design.md`.

## `[cas-tests-unchecked-optional-deref]` A test that dereferences a disengaged optional takes every later test in the binary with it {#cas-tests-unchecked-optional-deref}

A gtest that dereferences a disengaged `std::optional` does not fail — it aborts the process, and
every test scheduled after it in the same binary never runs. The gate then reports a smaller total
that still reads as green, so the regression that emptied the optional is invisible twice over: once
as its own missing failure, once as the suites it silently deleted from the run. This bit three times
in one night, each time presenting as "a suite disappeared" rather than as a failure.

The shape to write instead depends on the enclosing function's return type, and this is the part that
makes a blind `EXPECT_TRUE` → `ASSERT_TRUE` sweep wrong:

- **`void` test body** — `ASSERT_TRUE(x.has_value())` is correct and sufficient; `ASSERT_*` returns.
- **non-`void` helper** — `ASSERT_*` does not compile there (it expands to a bare `return;`). The
  helper must expect and then bail on its own: `EXPECT_TRUE(x.has_value()); if (!x) return {};`, or
  fold the guard into the value expression, `return x ? x->field : Field{};`. Both shapes already
  exist in the suite — `sealedCursorOf` and `holdOf` in `gtest_cas_gc_hold_grammar.cpp`, and
  `relinkTokenOf` in `gtest_cas_confirm_exact_ref.cpp` — and their comments state the reason.

**Measured on the branch at the time of writing**, not recalled: a scan for `const auto x = …`
followed within four lines by `x->` or `x.value()` with no intervening guard reports **13 candidate
sites** across 9 files, the largest groups being `gtest_ca_wiring.cpp`,
`gtest_cas_gc_frontier_gate.cpp`, `gtest_cas_orphan_nomination.cpp` and `gtest_cas_ref_writer.cpp`
(2 each). A first, naive version of the same scan reported 52 — the difference is entirely false
positives from shapes that ARE guarded: `if (const auto got = backend.get(…))`, and
`pending = e && e->delete_pending`. Any sweep must therefore be eyeballed per site, and the 13 are
candidates rather than confirmed defects; three were confirmed by reading
(`gtest_cas_lifecycle_condition.cpp:40`, `gtest_cas_orphan_nomination.cpp:180` and `:184`, each an
`EXPECT_TRUE` immediately followed by an unguarded `->`).

Separately, `EXPECT_TRUE(x.has_value())` appears 9 times against 401 `ASSERT_TRUE(x.has_value())`.
The `EXPECT` form is not wrong by itself — in a non-`void` helper it is the only option — but it is
the marker worth grepping for, because it is exactly where the author needed a guard and may have
stopped at the expectation.

The durable fix is not a one-off sweep: a sweep fixes today's sites and the next test written
reintroduces the class. What would actually close it is making the deref fail loudly at the point of
use — a checked accessor the CA test helpers use in place of `->` — so the shape is unavailable
rather than merely discouraged.

## `[gc-multidelete-conditional-gap]` batch `DeleteObjects` cannot replace GC's exact-token deletes as-is {#gc-multidelete-conditional-gap}

T9's destructive-baseline soak measured **944,155** individual `DiskS3DeleteObjects` calls across a
single 90-minute specimen's four destructive families (`pending_deletes`, `manifest_deletes`,
`ref_object_cleanup`, generation pruning inside `round_commit`) — every one a single-key
`removeObjectIfTokenMatches` call (`Backend::deleteExact`, `Backend/CasObjectStorageBackend.cpp:955`)
carrying an `If-Match` ETag precondition, the exact-token-match safety property that stops GC from
deleting a body a writer has already displaced (the CAS resurrection-safety invariant). ClickHouse
already has a working batch-delete path — `deleteFilesFromS3` (`IO/S3/deleteFileFromS3.cpp:80`,
default batch 1000, `IO/S3Defines.h:48`), reachable via `S3ObjectStorage::removeObjectsImpl` — but
no CAS delete-family call site uses it, including `deletePrefixWholesale`, which already LISTs a
whole prefix in pages and still deletes each listed key one at a time
(`Gc/CasGc.cpp:3563-3570`). The reason is not an oversight: the batch `DeleteObjects` request only
sets `Key` per `Aws::S3::Model::ObjectIdentifier` (`deleteFileFromS3.cpp:118-122`) — AWS's batch API
has no per-key conditional precondition, so wiring GC's existing calls to it as-is means dropping
the exact-token check, which is a correctness regression, not an optimization.

**Ceiling, if the conditional gap is ever closed** (e.g. a design that proves a delete cohort
collision-free at round-commit time without a per-key check): `944,155 → ⌈944,155/1000⌉ = 945`
batch requests, a >99.9% cut in delete request count. This is a REQUEST-COUNT ceiling, not a
wall-time prediction — the soak's backend (RustFS) measures ~650–700µs mean per-delete latency
(`DiskS3WriteMicroseconds`/`DiskS3DeleteObjects` ≈ 645µs for `pending_deletes` alone), far below
real S3 RTT, so the wall-time win against AWS S3 is unmeasured by this specimen and likely larger
than what RustFS would show.

**Falsification:** if no design can prove a cohort of exact-token deletes collision-free without a
per-key conditional (i.e. the safety property is fundamentally incompatible with a keys-only batch
API), this item stays permanently blocked and the correct scope is delete-side concurrency
(`[gc-delete-concurrency-serial]`) instead. Full measurement:
`docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-multidelete`.

## `[gc-delete-concurrency-serial]` GC's destructive deletes run with almost no overlap {#gc-delete-concurrency-serial}

The same T9 baseline measured `pending_deletes` and `manifest_deletes` running near-serially
despite already dispatching through a thread pool: `pending_deletes` wall (208.77s, ch1) is 87% of
the SUM of its individual requests' `DiskS3WriteMicroseconds` (181.3s) — the requests overlap very
little. `manifest_deletes` shows the same shape (409.52s wall vs. 368.56s summed, 90%). Together
these two phases are 618.29s of ch1's 4352.1s total phase wall (14.2%) in this specimen. A bounded
worker pool issuing K concurrent conditional deletes (same shape as the existing `meta_pool`) could
plausibly cut this toward `wall/K`, independent of `[gc-multidelete-conditional-gap]` — the two
levers compose (concurrent batch calls) rather than compete, once/if the conditional gap closes.

**Falsification:** if concurrent deletes against the same backend/prefix trigger throttling
(RustFS or S3 `SlowDown`/503) at a K nobody has tried yet, the real win is smaller than linear —
this baseline never issued concurrent deletes and cannot rule that out. Full measurement:
`docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-delete-concurrency`.

## `[gc-fold-intake-readbuffer-head]` `fold_ref_intake`'s HEAD/GET pairing is the generic read-buffer size probe, not the HEAD Task 15 already removed {#gc-fold-intake-readbuffer-head}

T9's baseline found `fold_ref_intake` — the single largest wall-time phase in a destructive round
(2303.0s of ch1's 4352.1s phase wall, 52.9%) — issuing `DiskS3GetObject` and `DiskS3HeadObject` in
an exact 1:1 pairing (1,183,381 each). This is NOT a regression of the predecessor's
`{#opp-fold-head}` (drop the HEAD in `foldManifestEdges`), which is confirmed delivered — the
source comment at `Gc/CasGc.cpp:1301-1312` states the HEAD was removed because the following GET
already carries the absence signal. The HEAD still visible here is a different, generic one:
`ReadBufferFromS3::getObjectSizeFromS3` (`IO/ReadBufferFromS3.cpp:463-469`) issues a `HeadObject`
to learn `Content-Length` before every ranged `GetObject`, for every S3 disk read in ClickHouse —
not CAS-specific.

**Not yet sized.** This entry only establishes that the pairing exists and where it comes from;
whether an existing known-size read-buffer constructor already avoids it on some call paths, and
what the real win would be, is unmeasured. **Falsification:** if the size-probe HEAD is required
for correctness on every generic S3 disk consumer (e.g. detecting a truncated/resized object
mid-read), this is a ClickHouse-wide question and does not belong on this CAS backlog at all. Full
measurement: `docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-fold-head-successor`.

## `[decommission-waits-on-the-wrong-predicate]` `cas_mounts` liveness and `NoWait` decommission disagree about what "dead" means {#decommission-wrong-predicate}

`SYSTEM CAS DROP POOL MEMBER` under the `NoWait` policy refused a genuinely dead node in CI
(`test_cas_drop_pool_member::test_drop_dead_pool_member_heals_the_pool`, PR 2073, integration
amd_tsan 4/6), 15.5 seconds AFTER the target's lease wall-clock expiry:

```
CAS decommission 'node2': pool member is alive or contended -- mount lease held by
uuid=... epoch=1 pid=10 hostname=node2 (expires_at_ms=1785811895007). Refusing ...
```

This is not a stuck lease. The two sides use different definitions of dead, and each is right on its
own terms:

- **The observable one** is wall-clock: `CasServerRoot.cpp:236` computes `live = !gc_fenced &&
  expires_at_ms > now_ms`, and that is what a `cas_mounts` reader sees. The same file's own operator
  text carries a `CLOCK SKEW CAVEAT` about precisely this comparison.
- **The one reclaim requires** refuses that comparison outright. `claimMount`'s comment
  (`CasServerRoot.cpp:410-424`) says a same-uuid/different-epoch lease is reclaimed "ONLY on a
  certificate of death that needs no fresh wall-clock trust -- never by comparing `expires_at_ms`
  against `now_ms`": `gc_fenced`, the clean marker, or a `proven_dead_token`. A `kill=True` stop
  leaves none of the three, and `NoWait` passes an empty `proven_dead_token` (`CasPool.cpp:668`),
  skipping the observation wait that would mint one.

So the only route to `NoWait` success for a hard-killed node is a GC round fencing the dead mount
first. In the failing run GC rounds were executing on their ~1s cadence but reporting
`deferred`/`candidates=0` — the fence had not happened yet. The test's precondition polls
`cas_mounts.state != 'live'` for up to 90s, which the wall-clock definition satisfies on its own, so
passing that gate does not establish what the call it guards actually needs.

**Not yet decided, and the decision is the work here:** whether this is a test that waits on the wrong
predicate (fix: wait for the fence, or use the waiting policy), or a product gap (fix: `NoWait`
decommission should accept a hard-killed member without requiring GC to get there first, or say in
its refusal what the operator must wait for). Do not "fix" it by weakening the certificate-of-death
rule — that rule is what keeps a live twin from being decommissioned across two clocks.

Falsification: if a rerun passes on unchanged code, it is a cadence race rather than a deterministic
gap, which changes the fix but not the mismatch.

## `[gc-round-budgets-are-not-backpressure]` Round budgets throttle the consumer while the producer is unaware — four defaults changed, the real fix is a time deadline {#gc-round-budgets-not-backpressure}

A per-round count cap is not backpressure. It bounds what GC does in one round while inserts and
merges — the producers of the work — know nothing about it. If arrival exceeds `budget × rounds/sec`,
the deficit is not smoothed, it accumulates. Whether that is harmless, degrading, or a leak depends
entirely on **what happens to the excess**, which turns out to differ per budget. Classified against
the code, not the names:

**A. Feedback loop (was capped, now unbounded).** `gc_round_graduation_budget`,
`gc_round_redelete_budget`. Excess is pushed back into `still_retired` "carry UNCHANGED"
(`CasBlobInDegree.cpp:472`), and the next round reads that list in full — `CasGc.h` marks the cost
`O(retired)`. So the round's cost grows with the debt while its useful work stays capped: rounds
lengthen, their rate drops, throughput drops, the debt grows faster. Worse than linear lag.

**B. Genuinely cursor-paced (unchanged, these caps are correct).** `manifest_sweep_list_budget_keys`,
`manifest_sweep_delete_budget_keys`, `gc_round_sweep_namespace_budget`,
`gc_round_sweep_recovery_op_budget`, `gc_round_prefix_wholesale_budget`. A cursor advances and never
regresses; a partially drained page or generation is simply finished next round. Nothing is
re-read, nothing accumulates. `gc_round_ref_cleanup_budget` is adjacent: it keeps no cursor but
`planRefCleanup` recomputes the same remaining candidates from durable state, so work is deferred,
not lost.

**C. A cap on one-shot work, i.e. a leak (was capped, now unbounded).**
`gc_round_handoff_prefix_wholesale_budget`. The struct's own comment says the hand-off "is a ONE-SHOT
event with no reclaimer behind it besides `fsck`: a generation it cannot fully reclaim this round is
never revisited (the parent-seal difference that triggers it does not recur)". This is the same shape
as the manifest-cleanup cap that was removed outright after a soak proved it leaked permanently.

**D. Audit loss (was capped, now unbounded).** `gc_round_outcome_entry_budget`. Nothing is retried on
exhaustion because the decision already happened; the only casualty is the audit row explaining it —
and it is dropped precisely on the busiest rounds, the ones an investigation would need.

**E. Not a throttle at all — an off switch (raised to effectively unbounded).**
`gc_frontier_probe_budget`. Exhaustion does not defer work: unprobed namespaces are simply unproven,
and one unproven namespace suppresses ALL destruction for the round (`CasGc.cpp:2047-2048`). It scales
with namespace count, i.e. with table count, so a value that is ample for ten namespaces becomes a
permanent GC stop for a large enough pool. **Its `0` cannot be redefined as "unbounded"**: unlike
every other budget here, `0` means "probe nothing", and the tests drive that exhaustion path
deliberately — so the default is spelled as a maximum instead. That inconsistency is itself an
operator trap and wants a proper sentinel.

**F. Memory bound, must stay capped.** `rebuild_edge_budget` — its comment is explicit that memory is
`O(budget)`, never `O(edges)`.

### What is still missing, and it is the real fix {#gc-budgets-need-a-deadline}

**A GC round has no time deadline anywhere in the code.** The count budgets have been serving as a
surrogate for one. That is why removing them is not free: a round holds the GC lease, and a round
that outruns the lease TTL gets fenced — the wedge class already fixed once in P3.1. The correct shape
is a per-round WALL-CLOCK deadline plus a cursor everywhere class A currently carries a list: the
round then does as much as it can inside its lease, stops cleanly, and resumes where it stopped
without re-reading the debt. Until that exists, the unbounded defaults above trade a silent
accumulation risk for a round-length risk, deliberately and with the user's decision.

Falsification for class A: with the caps off, a sustained-load soak should show round wall time
tracking arrival rate rather than climbing while `pending_condemned` climbs.
