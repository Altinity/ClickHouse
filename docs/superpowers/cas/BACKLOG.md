---
description: 'Consolidated live backlog of all still-pending CAS MergeTree work items. Single source of truth for what is left; issue IDs preserved (never renumbered). Groomed 2026-07-13.'
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

- **[gc-frontier-one-list] cheap change discovery: ONE LIST instead of per-namespace frontier GETs, + parallel walk** — DESIRABLE (USER-DIRECTED plan 2026-08-03) — {#gc-frontier-one-list} — Today every `Live`/`Removing` namespace costs the round at least one exact `GET cursor+1` (the frontier probe — the only thing that sets `frontier_proven`), even when `tail == cursor` says nothing changed, and the whole ref walk over `walk_targets` is a plain sequential `for` on the round thread (`Gc/CasGc.cpp:2134`; `meta_pool` parallelizes meta ops only) — so a quiet 10k-table pool pays ~10k **serial** GETs (~0.5 ms each) per round for the discovery alone. Plan, two independent levers: **(1) trusted-LIST frontier mode (per-backend, passported)** — on a store whose LIST is certified compliant (lists started after a completed PUT include it), the round's single frozen LIST tail at `tail == cursor` IS the frontier proof: a correct tail majorizes everything completed before the LIST started, and records landing during/after the enumeration are unacked by construction (ack requires the `_ckpt` CAS after the log PUT) and covered by the three temporal arms (`Gc/CasBlobInDegree.cpp:422`), so skipping the probe GET for quiet namespaces is sound — discovery cost drops to ~one paginated LIST per round. NON-NEGOTIABLE scope limits: the MIDDLE stays arithmetic unconditionally on EVERY backend (predecessor-omission below a correct tail is LEGAL S3 behavior under concurrent paginated enumeration — no snapshot contract exists for writes landing mid-walk — this is exactly the observed `0x1430c`/`0x1430d` shape); the probe survives for HELD namespaces (a hold clears only by exact-key resolution of its offending position, spec §5) and for namespaces with no listed logs at all (the hiding-store / quiescent shape); `tail < cursor` keeps feeding the store-quality detector. Certification = `Cas::Probe` LIST-consistency passport (§6 `[LIST consistency on real S3]` is the existing hook — connect it); RustFS today FAILS the passport (proven-by-measurement omission of 19-s-durable keys, `2026-07-26-list-incompleteness-proof/`), so conservative (probe-always) stays the default and the mode is opt-in per backend, same shape as `[ckpt-read-policy]`. **(2) parallel exact-key walk (backend-independent, no trust change)** — fan the per-namespace probes and the `cursor+1..tail` record GETs over a bounded pool (reuse/extend `meta_pool` or a sibling; per-namespace work is independent by construction — separate keys, separate cursors; fold application stays on the round thread or merges per-namespace results deterministically): wall time for N quiet namespaces drops N× serial → N/K. Lever (2) alone fixes the wall-clock without touching the trust model, and remains worth it under (1) for the hot namespaces that still fold records. MEASUREMENT: per-phase timing rows exist (`d412f85f749`) — record the discovery phase's GET count and wall before/after each lever; effect estimate on the quiet wide pool: discovery ~10k serial GETs → ~pages(LIST) requests (lever 1) or same GETs at 1/K wall (lever 2). — DESIRABLE — The single dominant remaining byte cost: a HOT pool rewrites the full snapshot run O(edges) per pass. Build O(delta)-write log-structured runs + periodic compaction on the landed T2/T0 primitives (streaming reader, `seek`, `getStream`, ranged `get`, seal-ref resolution). Canonical item (dedups: snapshot-streaming T1, ack-floor T1, 04/07 rows, refactoring-ideas "incremental LSM snapshot", O(buffer) run-file streaming residue). Streaming reads + reference-parent runs (T2/T0) already DONE.
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
- **[UNMATCHED-MINUS-ONE] unmatched removal-fold `-1` — VERIFIED HARMLESS (edge-set model); pin the property** — TEST (was briefly filed as HARD data-loss, 2026-07-23; corrected same day) — The suspected interleaving is real up to its last step: an ordinary `Precommit`'s activation-fold can hit a false-404 on a durable body (rustfs#3231 class) while the build is provably dead → dead-build skip eats the `+1` (`CasGc.cpp:1142`); the successor's stale-precommit sweep later appends the removal (`CasRefLedger.cpp:1958-1970`); the removal-fold reads the now-readable body and emits `-1` edges that were never `+1`-folded. BUT the terminal "in-degree under-count → premature delete" step does NOT fire: in-degree is a **source-edge SET, not a counter** — "Idempotent under re-fold at the merge (set membership, not a counter)" (`CasBlobInDegree.h:139`), applied last-wins per edge key (`CasBlobInDegree.cpp:574`, `:383-384`), and edge keys are per-(ref, ManifestId, path) — so an unmatched remove marks an already-absent edge absent and CANNOT touch другие manifests' edges. Consequence: harmless debris (orphan body + a no-op event), no data loss. This also refutes the terminal step of the codex finding-1 counterexample against the (abandoned) reserved-precommit design — that design was dropped for its OTHER confirmed problems (state-model sprawl, foreign-writer stageManifest bypass, tmp-fetch contract mismatch). REMAINING WORK: pin the load-bearing property with a cheap gtest (fold an unmatched `-1` and assert sibling edges survive + no condemn) so a future model change (set → counter) cannot silently reintroduce the class; optional TLA note REMOVAL-IS-SET-ERASE.
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

## 4. Read / write path {#read-write}

- **[ckpt-read-policy] modular `_ckpt` first-attempt view: conservative / cached / prefetch** — DESIRABLE (write-path; noted 2026-08-03, USER-DIRECTED design shape 2026-08-03: `_ckpt` handling must be modular/replaceable) — {#ckpt-read-policy} — Cost being addressed: since INV-4 landed (`eeadbc1887e`, Stage A), every committed ref-log chunk pays `GET _ckpt` + token-CAS *serially after* the log `PUT` (spec `2026-07-27-cas-ref-chain-complete-cut-design.md`); the two ref transactions of one part (precommit + commit) are serial phases and never share a chunk, so a **lone INSERT pays +4 serial RTTs** (2 GETs + 2 CASes); the pre-INV-1 id allocator was an in-memory `fetch_add`, i.e. zero S3, so nothing offsets this, and batching amortizes across concurrent parts only — the cost concentrates on low-concurrency insert latency. DESIGN SHAPE (user): a pluggable policy choosing only **where `publishCkpt`'s FIRST attempt gets its `{body, token}` view** — the invariant core is shared and policy-independent: retry-after-conflict ALWAYS does the whole-body exact re-read + re-merge (the "read the WHOLE body every attempt" rule in `Pool/CasRefCkpt.cpp` is about exactly this arm and stays), `lifeEpochWouldDecrease` runs against the view and again after any re-read (a stale token cannot win against a changed object, so detection defers at most one conflict round), and the durability order `log PUT → _ckpt CAS → ack` is untouched by every policy. Policies: (1) **conservative** = today: fresh GET per publish; (2) **cached** = seed with one GET on first touch, then serve the view from the writer's own last winning CAS (`RefTableRuntime`-scoped) and go straight to PUT-if-match — expected to essentially ALWAYS hit, because every `_ckpt` author of a live life (lane writer, snapshot publisher, recovery, genesis `completeCreation`) lives in the lease-owning process and cross-process writers are excluded by lease exclusivity; a miss is therefore a SIGNAL (fence loss / remount / foreign recovery), worth its own ProfileEvent, not noise; (3) **prefetch** = one paginated `LIST` over `cas/ns/state/` at mount seeds all `_ckpt` views, then memory-only + PUT-if-match — LIST here is a pure HINT and does NOT re-enter the trust model: an omitted/stale row just costs one lost CAS or one lazy GET on first touch, correctness rides the conditional write, consistent with LIST's demoted role. Mandatory cache-invalidation edges for (2)/(3): fence-generation change, wedge, remount supersession, `catalog_life_invalidated`. OUT OF SCOPE for the policy seam — always exact reads: recovery's `_ckpt` sample (memory is the thing under suspicion there) and GC-fold's frontier GET (possibly another process; needs the durable fact). Effect: +2 → +1 RTT per chunk, +4 → +2 per lone insert; the remaining CAS is irreducible (it IS the frontier mechanism, `NoAckedLoss`). MEASUREMENT PRECONDITION: the stage-1 figure **1.59x predates `_ckpt`** (measured 2026-07-24; `_ckpt` landed 2026-07-28) — re-run the wide-insert baseline on current HEAD first, then bench per policy against it. Per [[feedback_head_before_put_protocol_untouchable]]: no protocol step removed, nothing reordered in the durability chain, but it touches the commit path — ships as an explicit reviewed decision with before/after numbers; default policy choice (likely conservative until soak-proven) is part of that decision.
- **[write-path stage 1] parallel intra-part blob upload — LANDED (2026-07-24)** — The single-threaded serial blob-upload bottleneck of the wide CAS-on-S3 INSERT (documented in [[project_cas_insert_slowness_writepath]] and the point-4 baseline `docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md`) is now addressed by write-path stage 1: a server-wide `CasBlobUploadPool` + `fanOutBlobUploads` fan out a part's blob PUTs/dedup-HEADs (spec `docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md`, plan `docs/superpowers/plans/2026-07-23-cas-writepath-stage1.md`). Re-profile (`docs/superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect.md`): CA wide-insert wall **58.41s → 30.26s**, CA-vs-plain **3.0x → 1.59x**, top-thread Real share **72.3% → 14.5%**, CPU/wall **0.375 → 1.075** — the single-threaded signature is gone. Residual gap = the serial cross-part commit (ref batch still 1.0 BY DESIGN → **stage 2**, concurrent `commitPart` dispatch) plus the CAS-only dedup HEAD/GET traffic (~12% of Real wall — see `[B121 / B202 / one-GET-open]` below and the HEAD-before-PUT gate). Stage 2 is the next lever, now scoped against a smaller residual than the original 3.0x framing.
- **[TXN-ONE-PIPELINE] complete the "staging ops never defer" invariant** — HARD (small, structural) — The `01603` column-TTL abort (`de8a38b1e87`) was an ordering inversion between `DiskObjectStorageTransaction`'s two dispatch pipelines: eager (CA staging ops: `writeFile`/`createHardLink`/`moveDirectory`, and now part-file unlinks) vs deferred-to-commit (durable ops). A total order is impossible (read-your-writes B58/B63 and B151 force pre-commit effects; abort safety forces commit-gated durable deletes), so the correct invariant is *per-state-domain*: EVERY op that touches in-memory part staging must dispatch eagerly, leaving the deferred queue exclusively for durable non-staging effects (the two domains commute, closing the inversion class structurally). Residual gaps: `moveFile`/`replaceFile` in the part-file→part-file shape are still deferred (their B182 eager hook was deleted in T6 as trigger-less — the same "no trigger today" state `01603` was in before T8). Target shape (user direction 2026-07-15, option B): the deferred queue CEASES TO EXIST for CA — durable effects become staged INTENTS too (verbatim-file delete intent, part ref-drop intent, mountpoint delete intent) materialized at commit, and CA gets its own `ContentAddressedDiskTransaction` subclass dispatching every op straight to the metadata transaction in program order, deleting all four per-method `isContentAddressed()` branches from the base class (each of which was added through a bug: writeFile, createHardLink B58/B63, moveDirectory B151, unlink gate `de8a38b1e87`/`725dbc7d83c`). Delicate parts: verbatim writes are durable-immediate today (staged-write unification touches the mutation-MVCC read-modify-rewrite append) and autocommit one-shots keep their individually-durable contract. REFINED (user, 2026-07-15 evening): pair it with an explicit TWO-PHASE disk-transaction contract — new `IDiskTransaction::precommit()` (noop by default, zero change for ordinary disks); CA precommit = the ENTIRE publish (manifest from the overlay -> `precommitAdd` -> upload missing blobs -> promote), called before ZK-multi in Replicated (next to `renameParts()`) and before the `data_parts` lock in plain `Transaction::commit`; CA commit = durable-intent materialization only; abort-after-precommit = today's `dropRefBestEffort` compensation in an explicit phase. `moveDirectory` stops publishing (pure staging re-key) — B151's rename-window publish and the `rename_published_refs` machinery are DELETED. Verified basis: parts are `PreActive` until disk commit (no owner reads), the fetch handler serves `PreActive` but vanilla plain-s3 already fails+retries such fetches (queued rename), so the announced-but-unpublished window is an accepted upstream race, NOT a protocol obligation — the earlier 'publish must precede ZK announce' rationale was overstated. Contract decisions (user-settled 2026-07-15): `commit` IMPLICITLY runs `precommit` when it was not called (idempotent, flag-guarded — classic commit-implies-prepare; an assert would turn a positioning omission into a prod abort across dozens of commit call sites incl. the Keeper-recovery branch), WITH observability (ProfileEvent `CasImplicitPrecommitInCommit` + debug log) so a mis-positioned hot path surfaces in soak metrics; staging mutations arriving AFTER `precommit` are a genuine correctness violation -> fail-loud `LOGICAL_ERROR`. SCOPE ADDITION (user 2026-07-15): the refactoring must also DE-PATCH upstream code — remove the accumulated workarounds around eager-dispatch/read-your-writes in non-CA files; inventory with A/B/C classification + de-patching order = `docs/superpowers/cas/upstream-patch-inventory.md`. SPEC: `docs/superpowers/specs/2026-07-15-cas-txn-one-pipeline-design.md` (2026-07-15, approved brainstorm) — SUPERSEDES the staged-intents wording above: everything-immediate model (no intents — ABA rejection; local-disk abort semantics for verbatim/mountpoint deletes), single `dispatch` funnel in `DiskObjectStorageTransaction` (no CA subclass), generic `tryCreateWriteBuffer` hook, `precommit` call sites in `renameParts`/plain `commit`/freeze/restore/fetch; lands BEFORE codecs v3 and the source-layout refactoring.
- **[RELINK-CONFIRM-BUSY-LANE] fetch-by-relink is ~17% available under sustained writes** — {#relink-confirm-busy-lane} — REAL PRODUCT FINDING (2026-07-29 Stage A T14 soak, pre-existing behavior — NOT a Stage A regression: `confirmExactRef` refusal logic verified untouched by the stage, comment-only hunk) — `CasRefLedger::confirmExactRef` rule 3 (`CasRefLedger.cpp` ~:445) refuses `Unknown` while `wedge || !pending.empty() || leader_active`; a busy writer's lane is essentially never quiescent (sender at 168,955 `CasRefBatchFlushes`: ~92k refusals vs 19,531 proven ≈ 17% availability), so the receiver at `DataPartsExchange.cpp` ~:1550 abandons the relink with an ERROR-severity `NETWORK_ERROR` (~1000-3700/min, ~106k rows per 20 soak minutes, zero correctness consequence — replication converges, replica row counts equal). Fail-closed and CORRECT per the trust model; the finding is cost+noise+observability: (a) ANSWERED (2026-07-29, code read at the throw site): the abandon THROWS NETWORK_ERROR = the retry-later class — the replication queue stores the fetch, backs off, and RE-SELECTS on re-execution; a byte re-request to the same source is explicitly forbidden as unsound (comment at the row-3 throw). So refusals cost replication LATENCY + noise, never duplicate bytes and never double-publish; residual quantification = convergence-delay distribution under sustained writes; (b) USER-DIRECTED (2026-07-29): an `Unknown` that is OUR OWN uncertainty and part of the protocol must not report Error — demote severity AND rewrite the message text to name a TRANSIENT state explicitly (e.g. "relink confirm returned Unknown — an expected, transient outcome while the source lane is busy; the fetch is re-queued"), + ProfileEvents pair (proven/refused) + sender logs WHICH rule refused; AND the unhappy path gets its own PRECISE TEST — deliberately drive a refusal (pending item on the target name / wedge / fence-lost), assert severity + message class + no byte re-request to the same source + eventual convergence of the retry cycle; (c) the ledger's `Unknown` maps SILENTLY through `ContentAddressedMetadataStorage.cpp` ~:1996 — surface WHICH rule refused (took a live cluster to diagnose; a log grep should have sufficed); (d) design pass USER-APPROVED and DECIDED 2026-07-29: variant (ii) RE-OFFER chosen, the per-ref MutationScope index RETIRED pre-ship (see draft §0 for the full decision set: mandatory-recovery/discretionary-maintenance split, remote-recovery BUDGET w/ own counter vs the LRU-eviction amplifier, axis (iii) rejected on who-pays, NAMED TLA assumption, taxonomy rebuilt from the new state set); draft: `2026-07-29-relink-confirm-per-ref-draft.md`; rule 3 was table-scoped — any pending mutation refuses ALL refs; a per-ref refinement (confirm from committed rows provably unaffected by the pending window) could restore availability without weakening fail-close. Functional cross-check lives in `test_cas_replicated_relink`. MEASURED at scale (gc-audit 2026-07-29, `reports/2026-07-29-gc-perf-audit-soak.md`): 248,400 refusals in ~32 min cluster-wide, peak 9,219/min on one node, each paying an ERROR-severity exception with ~23 symbolized stack frames — a CPU profile of that soak would have measured the relink storm, not GC.
- **[B121 / B202 / one-GET-open] read request-count reduction** — DESIRABLE (design pass) — B202 inline-by-size (drop the file-type predicate, inline < ~512 KiB, weigh the wide-part-medium-column regression, `.bin` carve-out) + a per-blob-GET read-cost reduction (B121) + one-GET part open (pack small files). Pure perf/request-count; no safety dimension. Companion to the (landed, opt-in) file-cache disk for re-read-heavy workloads.
- **[B10] `manifest_size` always 0 in `Resolved`** — MINOR — `Store::resolveRef` hardcodes `.manifest_size = 0` (`CasStore.cpp:844`, twin at `:995`). Now a **live consumer**: `PartFolderView::weight()` = `bytes + manifest_size`, so the part-folder cache weight silently under-counts the manifest body. Set it.
- **[B98] Streaming `putOverwrite` (condemned-displacement)** — DESIRABLE — The rare INV-1 revival/displacement path still materializes the whole body; not a blocker.
- **[promote-recreate] promote-time in-place recreate of a condemned SOURCED (tokened) blob** — DESIRABLE — The tokened promote gate stays fail-closed `ABORTED`; recreate happens on the retried build via `putBlob` cold-reuse. The tokenless-evidence copy-forward case is DONE. Ideal root-cause fix (writer-triggered synchronous fold-barrier at promote) is blocked by the lack of a writer↔GC synchronous-fold API — deferred behind the landed bounded resurrect.
- **[R1/X1] ephemeral reader pin (cross-node GC fence)** — DESIRABLE / VERIFY — Per-server-owned namespaces narrow the window and a live ref resolving to an absent object surfaces `FILE_DOESNT_EXIST` (INV-NO-DANGLE), so for normal MergeTree this is covered by DataPart lifetime; the ephemeral-pin mechanism is design-only. Audit whether any ref-less/cross-node reader path exists before implementing.
- **[B85] read-path 404 auto-repair** — VERIFY — Read-path 404 currently surfaces as a hard error (INV-NO-DANGLE). Auto-repair-on-404 was an open resilience idea; confirm whether still wanted post-v3.
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
- **[B125] integration tests on RustFS (not MinIO)** — GATE — Current integration tests use MinIO; production uses S3-compatible backends that enforce conditional-PUT.
- **[B131] repo hygiene + M-W comment sweep** — GATE — 30 dangling `M-W`/`D-W1`/`2026-06-12-ca-core-m-w` comment references across 13 src files (incl. `ContentAddressedMetadataStorage.{h,cpp}`, `CasGcScheduler.h`, `DataPartsExchange.cpp:106`) reference the deleted plan — sweep to self-contained wording. Non-shippable files: `poc/cas_mergetree/` already deleted (F1 landed); the untracked empty `poc/` husk remains.
- **[B15/B99/B169/B159] `system.*` views for pool/blob/part refcounts + `clickhouse-disks` decode/introspect** — HARD (PARTIAL) — GC log + event log + `content_addressed_mounts` + ca-fsck/dryrun/rebuild/ca-inspect CLI done; per-part/ref `system.*` views + a top-down decode/traversal surface not yet. (INTROSPECTION-1/2 close signals.)
- **[B13] migration path for existing tables** — HARD — `ALTER TABLE … MOVE PARTITION` to a `content_addressed` disk re-packs; mixed-version rollout rule (read-new-before-write-new; format self-check fails closed) + a rollout-safety spec.
- **[B48 / B167a/f] clean shutdown** — RESOLVED (verified 2026-07-23) — **B48 no longer reproduces**: `clickhouse local` + CA disk (local backend, `gc_interval_sec=2`) exits cleanly on today's release AND ASan builds across all variants — immediate exit, ~9s alive with GC rounds on the dedicated thread + `OPTIMIZE FINAL` + `DROP`, exit with the table still alive (no `DROP`), and re-open of an existing pool; trace log shows active `CA GC round N` ticks and `Destroying disk content_addressed` ~34ms after the last round (repro kept in `tmp/b48_repro/`). Fixed structurally by the rev.8 disk-lifecycle round (GC-scheduler self-exit + fail-loud teardown). **B167a/f code-verified as implemented**: `Context::shutdown` (`Context.cpp:1047`) → `DiskObjectStorage::shutdown` (`DiskObjectStorage.cpp:511`) → `ContentAddressedMetadataStorage::shutdown` (`ContentAddressedMetadataStorage.cpp:834` — waits out an in-flight synchronous GC round, detaches under `pointer_mutex`, then `stop()` joins the GC worker+heartbeat threads) → last pool ref drop runs `~Pool` (`CasPool.cpp:745` — stop remount thread → bounded `drainRefLanesForShutdown` → `CasMountRuntime::finishTeardown`), which on a certified drain has the keeper terminal op stamp the lease already-expired + fold in the watermark farewell (`min_active = UINT64_MAX`, `MountRelease`/"farewell" audit event), and on an uncertified drain fail-closed skips the clean marker (successor uses observation-based reclaim). Inline `disk(...)` disks are registered via `Context::getOrCreateDisk` into the SAME selector map the shutdown loop iterates, so they are covered. Out of scope here (tracked separately): hard-kill leaves no farewell by design, and DROP-TABLE disk eject remains the disk-lifecycle-leak item.
- **[F1-prod] read-only same-pool shadow disk (`ca_ro`) breaks table load on restart** — GATE (prod) — MergeTree part discovery finds every part twice → `UNKNOWN_DISK` on restart with CA tables. Stand workaround shipped (standalone `clickhouse-disks -C` fsck-only config; propagated to the default stand); PRODUCT fix (part discovery skips `readonly` same-pool disks, or a `hidden`/`introspection_only` disk flag) still open; `10replicas`/`gc_shards2`/`awss3` server configs may still embed `ca_ro`.
- **[B165] server OOM at hour-4 soak (~49 GiB RSS)** — VERIFY — Not reproduced since the `putBlob` streaming fix; re-run a long soak to confirm resolved.
- **[SEC-1] trust-domain documentation** — DOC — Document "one CAS pool = one trust domain" (CityHash128/XXH3 are not cryptographic; dedup is cross-tenant within a pool). For a multi-tenant future: crypto hash mode or trust-domain-scoped dedup (sha256 mode now exists as a building block). SEC-2/SEC-3 are by-design under this model.
- **[AD-3] day-2 runbook** — DOC — Table of failure mode → signal/metric → diagnostic command → recovery command → test (stalled GC, persistent clamp, lost/corrupt `gc/state`, live mount conflict, orphan refs/manifests, pool-meta corruption, control-plane backup/restore).
- **[B14] expedited / GDPR right-to-erasure delete** — DESIRABLE — Under GC lock, confirm no live ref, then delete bypassing the two-phase graduation delay; no layout change.
- **[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation.
- **[B26 / B135] [B66a] → §14** — local/emulated-backend items collected into §14 {#local-backend} (2026-07-23 grooming, user direction: local-backend stories live in ONE section).
- **[B66b] relink-into-detached (zero-byte `to_detached` fetch for same-pool parts)** — IN PROGRESS (2026-07-23) — folded into the publish-confirm fetch-handoff iteration (spec `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md`): relink already publishes under `tmp-fetch_<part>` and re-keys via `renameTempPartAndReplace`, so detached needs only lifting the `!to_detached` advertise gate (`DataPartsExchange.cpp:540-545`) + the detached temporary name + the same confirm step; collision semantics inherited from the byte path by construction. (RPL-4 perf cliff.)

## 8. Mount-lease / fence recovery {#mount-fence}

- **[P3.1 Task 6 / S13] live validation of fence-recovery** — TEST — TLA+ gate PASSED and the correctness paths landed (self-remount on GC fence-out is DONE); the gtest sweep + S13 3×-green live gate remain. **Task 5** (decouple renewal from the retired-view sync beat) is likely **MOOT** — freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`; confirm and close.
- **[A7-residual] gc_scheduler lifetime vs manual rounds** — VERIFY — Believed addressed by `89845c2a544` (shutdown serializes gc_scheduler teardown with health reads; wedged-lane count pinned) on top of the stabilization A7 fix. Confirm no residual: (a) a manual round on a raw pointer captured outside the lock, (b) lazy creation resurrecting a scheduler after shutdown.
- **[codex-6] wire the fetch-handoff retention pin** — HARD (spec exists, not wired) — The relink sender is fire-and-forget (`DataPartsExchange.cpp:255-280` streams manifest bytes and releases the source part): if the receiver's `precommitAdd` edge-PUT stalls across ≥2 GC folds while the source part goes `Outdated` with no other ref, the blob is reclaimed → a dangling committed manifest (fsck-detected, not silent). The gap is self-documented at `ContentAddressedMetadataStorage.cpp:1335-1343` and re-confirmed by the 2026-07-17 codex-review triage (finding №6; token-CHANGE recoveries are already covered by the GC `deleteExact` liveness re-check — only this same-token tail is open). Fix (REVISED again 2026-07-23, evening): **publish-then-confirm** — the receiver publishes its manifest + precommit exactly as today (own domain, `+1` durable with a present body), then asks the sender `confirmExactRef` (one read-only interserver RTT; **exact-token + lane-linearized** per spec rev.2 — compares the exact `ManifestRef`, answers pessimistic *unknown* on any wedge/pending append/cold table, never through plain cached `resolveRef` — closes the round-2 review's stale-cache and ABA-repoint counterexamples) and promotes only on *yes*; *no*/unreachable → abort precommit + retryable fail to the replication queue (source re-selection / covering part — NOT a byte re-request to a sender that has nothing). EDGE-BEFORE-OBSERVE lifted to part level; ZERO GC/codec/sweep changes. Spec `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md` (supersedes the retention-pin spec; the intermediate reserved-precommit design lived a few hours and was REMOVED after a codex gpt-5.6-sol xhigh adversarial review proved it unsound — late foreign PUT after dead-build skip ⇒ unmatched `-1`; git history is its archive). Also covers B66b + the RPL-5 queue-clone test slice. Interim alternative unchanged: gate relink off to the byte-fetch path until it lands.
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
- **[B5] reconcile shared-pool integration tests to the per-`server_root_id` tree** — TEST — Integration tests still assume the old shared-pool layout.
- **[ci/full-scale sweep] run dev-scale inconclusives at designed scale** — TEST — RSS attribution, manifest caps, scenario S01–S35 at ci/full.
- **[CA-s3 stateless lane] full-lane run + remaining un-tagging** — TEST (PARTIAL) — Point-fixes landed (04286/05008/05009/01271/03829, B86 removed); run the full lane. Un-tag the remaining `no-content-addressed-storage` tests now that B31 (capability gate) is closed. 3 pre-existing `CaWiring*` GC/shadow gtests fail identically (re-exposed when the sweep filter widened) — root-cause + fix or re-gate.
- **[CI-P1] RustFS provisioning for the CA-s3 functional lane** — INFRA — Add a tracked `setup_rustfs.sh` (mirror `setup_minio.sh`) invoked before the job's `start()`; today it only checks for a pre-existing `ci/tmp/rustfs`.
- **[soak-harness minors]** — INFRA — TTL-band oracle widening for long runs; unreliable pool telemetry at scale (`pool_objects`/`pool_bytes` None); `run_24h.sh` destroys prior-run raw logs at start (move → `logs/prev_<ts>` after a run); S24 needs a pre-agreement `SYSTEM SYNC REPLICA`; S01 scratch high-water sampler misses the OPTIMIZE-FINAL spike; `s3cache` scenario flip to a positive cache-hit assertion; scenario README/cards still say `root_shards` after the S08 oracle rewrite.
- **[S16] strict resurrect-count check may fail deterministically** — TEST (watch) — If `forced_gc_to_fixpoint` fully deletes bodies, re-insert is a plain `blob_put` and `resurrect_count>0` fails — scenario-cycle tuning, not a product bug.
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

## [GC-THROUGHPUT-COLLAPSE] dropped namespaces are never pruned once GC rounds stall — 63% of the CI pool was corpses {#gc-throughput-collapse}

Found 2026-07-24 by asking a question the first analysis had waved away ("isn't that LIST just the
live refs of many parallel tests?"). It is not. Measured from the PR-2073 CA-s3 stateless run's
server log:

- of the 167 distinct ref-page boundaries the slow LIST walked, **128 distinct namespaces; 113 have an
  explicit drop record and 97 were dropped BEFORE the query even started** (median 25 min earlier,
  oldest 58 min). Weighted by page boundary, **63% of the listed volume sits in already-dropped
  namespaces**; the only undropped ones are the server's own `system.*_log` tables;
- 15,123 namespaces existed before the query, **9,538 explicitly dropped**;
- the pool ran **32 GC rounds in 95 minutes and then none at all for the last ≥47 minutes**;
- the rounds were losing: candidates 1006 -> 4841 -> 20046 while deletions stayed ~700/round, and the
  round interval went 9 min -> 28 min -> never.

MECHANISM — corrected twice, so here is only what is ESTABLISHED versus what is not.

Established from the code: a round is all-or-nothing. `fold` (O(universe)) -> ONE `gc/state` CAS
(`CasGc.cpp:636`) -> cleanup (`runNamespaceCleanupPasses` + `cleanupRefObjects`, `:725-727`). Cleanup
sits after the commit point BY DESIGN — a `Removed` snapshot must be durable before the logs it covers
may be deleted ("ORDER IS LOAD-BEARING", `:713-724`) — so any exit before the commit does ZERO
cleanup, not partial cleanup. Rounds are serialized by the scheduler (`gc_round_mutex`,
`gc_interval_sec=5` on this lane), so the next round only begins when the previous one RETURNS.

NOT established, and previously asserted here in error: that a competing leader stole the lease and
made the CAS lose. This lane runs a SINGLE server (`server_root_id=stateless-ca-s3`), so there is no
second replica to compete; and stealing from a LIVE incumbent is refused by construction (manual
rounds pass `allow_steal=false`; the loop's steal exists for DEAD-incumbent recovery). The lost-CAS
path at `:638` is real code but there is no evidence it fired here — the discriminator is whether
`gc/state moved during the round` appears in the log, which was never checked.

Most parsimonious reading of the evidence: ONE round was still folding. With ~15k namespaces and
20,046 candidates, a single round exceeding 47 minutes needs no contention to explain, and because
rounds are serialized no further round could start. Cleanup did not run because the round never
reached its commit point — a fold-COST problem (§2 `[Lever B]`, the O(pool) round), not a leadership
problem. Either way the loop is the same: no cleanup -> dead namespaces persist -> universe grows ->
fold slower.

SEPARATE OPEN QUESTION worth checking, because it would make catch-up impossible on its own:
deletions stayed flat near 700/round while candidates grew to 20,046. There IS a per-round budget in
this area (`manifest_sweep_delete_budget_keys`, default 100, `CasGc.cpp:1463`) — determine whether the
blob/manifest deletion path is likewise capped per round. A fixed per-round delete budget against a
growing candidate set can never converge once it falls behind.

NOT the same as `[codex-11]` (an EMPTY Live-but-ownerless ref-table revived through an
allocate/register TOCTOU): here the namespaces were cleanly dropped, with populated ref-table logs
left behind. This is a cleanup-THROUGHPUT failure and belongs with the GC scalability family
(§2 `[Lever B]`, the O(pool) round cost, the quadratic-LIST scenario finding) — Lever B's change
signal is exactly what would stop a round from re-walking an unchanged universe.

Why it matters beyond CI: the same feedback loop applies to any long-lived pool with table churn.
Evidence caveat from the analysis: 167 pages is a LOWER bound (the client only logs a URL when it
retried), so the absolute volume is larger; the dead/live ratio is unaffected because boundaries
sample uniformly by object count.

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

## CLOSED 2026-07-17: S37 chaos-leg oracle bug — self-grounding oracle landed (`a1e27178ba6`), first VALID mid-policy-MOVE-kill verdict is GREEN
Resolved by the GREEN-DEBT #22 fix: the card now reads `rows_before_chaos_insert` and expects `rows_before + ttl_rows` (self-grounding) instead of the fixed param, plus bounded `PART_IS_TEMPORARILY_LOCKED` retry on the MOVE leg. Validated: dev rerun PASS 23/23 pre-hardening (build/test_s37_greendebt_run2.log) AND post-hardening committed-card run PASS 23/23 (RUN_HISTORY 20260717T173612_S37_seed1) — the first genuine atomicity verdicts, NO product defect. Historical entry below.
## (historical) VERIFY: S37 chaos-leg oracle bug (rows==100 vs true 200) — the mid-policy-MOVE-kill atomicity property has NEVER actually been verified (sharpened 2026-07-17)
The "restart mid-policy-MOVE is atomic" verdict has failed byte-identically on every run since the R2 landing (26590e4aa55f -> today), and today's post-DL-fix run shows `mover_error=None, checksum_stable=True, rows=200` — exactly 100 (TTL leg) + 100 (chaos leg insert into the SAME un-truncated `s37_ttl`), with the data checksum unchanged across the kill. This confirms the 2026-07-16 "minor/unconfirmed" note: the card's expectation compares against the fixed `ttl_rows` param (100) instead of the true expected 200 — a scenario-oracle bug that has been MASKING the real property. ACTION: fix the card (truncate before the chaos leg, or expect 200), re-run S37; only then do we learn whether a genuine mid-MOVE-kill atomicity defect exists. Until then S37=22/23 is a harness artifact, not a product verdict — but also NOT evidence of safety.

## GREEN-DEBT: S39 ci-scale config bug — `short_fault_s=15 >= _MOUNT_RENEW_PERIOD_S=10` invariant violation at ci scale (found DL-fix T4, 2026-07-17)
S39's ci param row bakes in a short-fault window that violates the card's own timing assumption (the "short" leg must complete its fault inside a renewal period for the no-fence assertion to be sound); dev scale is correct and is what all prior green runs used (ci scale had NEVER run per RUN_HISTORY — this red was latent since the card's creation e93c28a17694a1). Fix the ci row in `s39_lease_fault_tolerance.py` param_table (size ci off the same 30s TTL anchor as dev, keeping short < renew period) and run S39 at ci scale to green. Per the no-known-reds rule this is a tracked return-item, not a "known".

### FIX LANDED 2026-07-17: generic renameParts disk-transaction close (spec 2026-07-17-part-durability-before-keeper-commit-design.md)
`77484196b0d` closes every part disk-storage transaction in `MergeTreeData::Transaction::renameParts` (part durable BEFORE the Keeper block_id registration; disk commit moved off the `data_parts` lock). Regression test `05014_insert_dedup_disk_commit_failpoint` + targeted failpoint `part_storage_fail_commit_transaction` (`2c1b15ed4ae`): pre-fix count=0 (silent loss), post-fix count=1. Gates, ALL GREEN on the fixed binary: S40 (new permanent card, `e302c36421f`) PASS 10/10 with acked=3796 lost=0; dl_probe (tracked `utils/ca-soak/tools/dl_probe.py`) LOST=0 (pre-fix ~198/1314); S39 dev 11/11; S36 26/26; 20m seed-42 soak (the original R4 chaos recipe that lost 1118 rows) PHASE3 OK, checkpoint deficit ZERO, dangling=0. S37 22/23 = card-oracle artifact (GREEN-DEBT, see the VERIFY entry). **R3 (#37) ship-readiness RESTORED.** Residual narrower hazard (block_id outliving a durably-committed part lost later) stays out of scope — verify-on-dedup is the candidate if it ever matters. Upstream submission draft: `tmp/upstream_issue_dedup_durability.md` (pending user decision).

## GREEN-DEBT: local build-dir config drift — ALL localize_rust_c_* rules in build/build.ninja lost their reference-library args (found 2026-07-17 at image build)
`ninja -C build clickhouse` now fails at `localize_rust_c_chdig` ("Error: no reference libraries given"); inspection shows EVERY localize rule (chdig, polyglot, wasmtime, delta_kernel_ffi) in the generated build.ninja carries only 4 args (lib/ar/objcopy/nm) and zero refs — while corrosion-cmake's generator only creates these targets when `_localize_ref_libs` is non-empty (contrib/corrosion-cmake/CMakeLists.txt:319-415). So some past reconfigure generated with a state where the genexprs produced nothing — latent until today because the rust targets were never dirty in recent incremental builds (T2's build passed because its cargo steps rebuilt but chdig localize did not re-run... first execution today failed). Impact: any future `ninja` touching rust contribs fails in build/. Fix on resume: full cmake re-configure of build/ (then check one localize rule has refs) — and understand WHICH configure produced the argless state (guard against recurrence). The 2026-07-17 nightly image was built from the T2 binary (10:35, all DL-fix gates ran on it) — unaffected.

## FIXED 2026-07-18: `CasPartWriteTxn.ManifestCapEncodedBytesJustUnderStagesSuccessfully` real-clock/TSan speed artifact
This boundary test (constructs a manifest just under the 256 MiB `kExpectedManifestEncodedCap` and stages it) failed deterministically under TSan (3/3 reruns, ~24-26s each) with `stageManifest: part-manifest PUT ... UNCERTAIN (retry budget exhausted)` — no `ThreadSanitizer` warning anywhere, ruling out a genuine race. Root cause: `openPool()` in `gtest_cas_part_write.cpp` did not inject a fake `boot_ms_fn` (unlike other deadline-sensitive tests in this suite), so `CasMountRuntime::refAppendFenceOk` gated every controlled attempt against the REAL wall clock (mount_lease_ttl_ms=30000, safety margin 7000ms -> fence trips ~23s after pool-open). The pre-retry-loop encode+seal of a ~256 MiB manifest body, ordinarily ~4.3-4.9s (passed reliably under plain builds and under ASan), ran long enough under TSan's per-memory-access instrumentation to approach/exceed that ~23s real-clock threshold. Verified NOT a regression at the time (none of the 2026-07-18 fix-wave + final-review commits touched `CasMountRuntime.cpp`'s fence logic or the manifest-encode path).
**FIX** (`47ea8f3c1d9`): the test now opens its Pool with a frozen `boot_ms_fn` (`[] { return uint64_t{0}; }`) instead of the shared `openPool()` helper, decoupling both the mount-lease fence and the CAS request controller's own deadline math (both consult the same injected clock seam) from real execution speed. Verified 3/3 green under TSan (~22-23s each, real CPU time, no longer racing a lease deadline) and unchanged under ASan/plain.

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

## Ref-ledger follow-ups from the two-model adversarial consult (Fable max-depth + gpt-5.6-sol high, 2026-07-21) {#ref-ledger-consult-followups-2026-07-21}

Companion of the fail-closed restore (un-elided cross-owner check + snapshot validation + container
hardening + `ApplyMode` privatization) that landed the same night. The three items below were
consult-flagged, controller-verified, and deliberately DEFERRED with measurement/design gates —
per-item severity and evidence in `docs/superpowers/reports/2026-07-21-reftablestate-experiments.md`
and `tmp/consult-gpt56sol-answer.md`.

- [ ] **HARD (design): post-durable-PUT allocation window in the ref-lane flush** (gpt-5.6-sol F2).
  After the object store confirms the ref-log PUT `Committed`, the leader still performs allocating
  work (`applyRefLogTxn` live install, `materializeCommitted` copying both COW containers) inside the
  same critical section; a `std::bad_alloc` there is reported by the catch as "permanently
  unreplayable history" and completes every waiter with an error for a transaction that IS durable —
  a retry then legitimately observes "already exists"/"absent" splits. Pre-existing shape (predates
  this round; E2's second container widened the window marginally). Fix direction (consult-endorsed):
  construct + fully materialize the exact candidate state BEFORE the PUT; after durability, install
  via a verified no-throw move under `state_mutex` (static_assert on noexcept), then update tail
  counters. Touches the ledger flush ordering — needs its own spec + soak gate. UPDATE 2026-07-23:
  the publish-confirm fetch-handoff review (round 3) independently rediscovered this window as the
  one state breaking `confirmExactRef`. UPDATE 2026-07-24: **FOLDED (user direction) into the
  unified publish-confirm spec** `2026-07-23-cas-fetch-handoff-publish-confirm-design.md`
  §ledger-hardening — the poison marker (fail-closed half) is its item 1 / execution phase 3, the
  full no-throw-install (this item) is its item 3 / execution phase 7 with the measurement gate
  (wedge→flush bench + `BM_FlushInstall`) as the phase's first task. Tracked THERE now; this entry
  kept as a pointer.
- [ ] **DESIRABLE (measured, est. 2-3× recovery/GC-rebuild cut): recovery re-runs 3-4 codec passes per
  snapshot row** (Fable F5). `recoverRefTableDetailed` decodes the snapshot, then `stateFromSnapshot`
  re-encodes + re-decodes it (hand-built-snapshot defense), then per-row size helpers re-encode
  fragments; the E3 report's "residual O(N)" replay constant is mostly this. Fix direction: a
  validated-witness type produced by `decodeRefTableSnapshot` that `stateFromSnapshot` accepts
  without the round-trip (hand-built callers keep the round-trip path); micro-bonus: seed
  `snapshot_body_bytes` from `bytes.size() - snapshotFramingSize(...)`. Gate: benchmark
  recovery-then-first-append before/after.
- [ ] **VERIFY-then-maybe (measured): `precommits` is a plain `std::set`, deep-copied per state copy**
  (both consults, independently). Every `RefTableState` scratch copy is O(P) string copies; P is
  bounded only by the admission byte budget (up to ~64 MiB encoded), not by the 1,000-op txn cap;
  every shipped "copy O(1) ~58 ns" number used a ONE-precommit fixture. BOTH consults: do NOT build
  a third COW container without a number. First step: extend `BM_ScratchCopy`/`BM_Admits` with a
  P-sweep (P=1/100/10,000) and check a precommit-heavy soak phase; only a real measured cliff
  justifies a `RefCowPrecommitSet`.

- [ ] **DESIRABLE (defense-in-depth; MANDATORY before any multi-writer or rolling-upgrade-skew
  milestone): GC per-table recovery gate before ref-log fold** (round-2 consult disagreement,
  resolved by two-model refutation 2026-07-21). The GC fold extracts manifest edges from decoded
  logs without state-machine replay (`CasGc.cpp` ref intake; designed intake model, pinned by
  `gtest_cas_gc_undercount_repro.cpp`); a codec-valid but semantically-fabricated removal of a LIVE
  edge would fold a wrong `-1` and can premature-delete (source-edge identity carries no ref owner).
  REFUTED as a live defect for the current branch: the single lease-holding writer structurally
  cannot mint such history (validated-prefix argument), raw appenders are test-only, S3 tamper is
  out of trust model — and the 2026-07-21 fail-closed fix NARROWED this surface (recovery now
  rejects any history no valid state machine could produce) and is the prerequisite for the gate.
  Fix shape (consult-endorsed): per-table `recoverRefTable(ns)` BEFORE folding that table's new
  logs; on `CORRUPTED_DATA` clamp the table (no cursor advance, no delta merge, anomaly recorded —
  same per-table clamp discipline as missing bodies), continue other tables. Abort-only ⇒ fold
  determinism preserved. Cursor-aligned witness replay (the other proposed shape) is INFEASIBLE:
  snapshots publish independently of the GC cursor and are routinely ahead of it. Cost: one
  recovery per table per round (orphan sweep already pays exactly this; share the GETs).
- [ ] AMEND the "post-durable-PUT allocation window" item above with two round-2 nuances: (a) the
  catch's "provably unreachable / permanently unreplayable" framing is an over-claim — the covered
  region includes `materializeCommitted`, which CAN throw in production via `MemoryTracker` limits,
  yielding a wrong LOGICAL_ERROR diagnosis for a durable+applied transaction (narrow the framing to
  the apply step when restructuring); (b) wedge resolution applies to the retained state without
  materializing either COW container — bounded, but a post-wedge flush copies an unmaterialized
  overlay, a path `BM_FlushInstall` does not model; measure wedge-resolution-followed-by-flush
  before changing anything (naive post-durable materialize would add an allocation failure mode
  between apply and unwedge).

## Disk-error (ENOSPC / inode-exhaustion) audit follow-ups (8-agent sweep + controller verification, 2026-07-21) {#disk-error-audit-followups-2026-07-21}

Findings of the staging/target/GC disk-error audit (staging `cas_scratch`, Native S3 target,
`EmulatedSingleProcess` local target, GC round, read path). Overall verdict held: staging ENOSPC is
fail-loud with no pool side effects; Native S3 stays corruption-free (atomic PUTs, fail-safe error
classification in `finalizeConditionalWrite`, bounded `CasRequestControl` retries, fail-closed
fencing); GC is decision-durable-before-delete with `suppress_destructive` on every corruption-
tolerant fold branch. The items below are the residual gaps, ordered by value.

- [ ] **HARD: size guard at dedup-admit** — `PartWriteTxn::observeAndAdmit` (4-arg overload,
  `CasPartWriteTxn.cpp:276-288`) checks only `hr.size >= blob_header_len`; it never compares
  `hr.size - header_len` against the caller's expected `source.size`, and `putBlob`'s dedup-hit
  result is discarded by the transaction (`ContentAddressedTransaction.cpp:281`). A truncated
  object sitting at a content-addressed key (possible on the emulated/local backend, see next item)
  is admitted as a dedup hit and produces a durably unreadable part. One cheap comparison closes
  the whole truncation-admit class on every backend (the HEAD-first path, the post-412 revive
  observe, and the 3-arg gate path all funnel through this overload).
- [ ] **HARD: temp-file + rename in the local blob write path** — moved to §14 {#local-backend}
  (2026-07-23 grooming); paired with the size guard above as defense-in-depth.
- [ ] **DESIRABLE: fsck physical-size check for blob bodies** — `runFsck` HEADs every blob but
  never compares the physical size against `blob_header_len + entry.blob_size`, so a truncated
  blob passes as `Reachable` (`CasFsck.cpp:371-414`). The listing already carries the sizes — the
  check adds zero extra requests. Payload re-hash against the content address stays a separate,
  opt-in deep mode (today NOTHING re-hashes on read by design — blob-body integrity is delegated
  entirely to MergeTree `checksums.txt` / compressed-block checksums / `CHECK TABLE`).
- [ ] **DESIRABLE: free-space guard + orphan sweeper for `scratch_path`** — the local staging write
  path has no `IDisk::reserve`, no `statvfs` check, and MergeTree reservations cannot see the
  scratch filesystem (the CA disk is object-backed, the scratch dir lives on the server data
  path). Peak scratch usage = whole-part size × concurrent part writes (all pending blobs of a
  part are held until `commit`). Also: orphaned `<rand>.tmp` files survive an unclean restart
  forever — disk init only does `fs::create_directories`
  (`MetadataStorageFactory.cpp:238`); the S3 staging prefix has `sweepOwnMountStaging`, the local
  scratch dir has no sweeper at all. Minimum: document the sizing rule; better: a pre-write
  free-space check plus a startup sweep of stale `*.tmp`.
- [ ] **MINOR: wrap the GC post-CAS cleanup in try/catch** — the post-CAS owner-removed
  manifest-body `deleteExact` loop (`CasGc.cpp:691`) and the hand-off `deletePrefixWholesale`
  (`:677`) are not wrapped; a genuine backend error (5xx / storage-full) there escapes
  `runRegularRound` AFTER the round's `gc/state` CAS committed. Data-safe (decision durable;
  leaked bodies reclaimed by the orphan-manifest sweep) but it skips the rest of that round's
  post-CAS cleanup and reddens the round. The orphan sweep itself is already wrapped
  (`:728-735`) — extend the same containment to its two siblings.
- [ ] **DESIRABLE: GC scheduler backoff + a distinct storage-full signal** — the pacing loop
  retries a failing round at a fixed interval forever with no backoff, no failure counter, no
  circuit breaker (`CasGcScheduler.cpp:255-261`); the only operator surface is
  `last_success_age_seconds` in `system.content_addressed_mounts`. And no ProfileEvent
  distinguishes "target storage full" (S3 507 / `XMinioStorageFull` / local ENOSPC) from generic
  instability — both look like `CasConditionalWriteUnresolved` + rising staleness. Add
  capped-exponential backoff on consecutive failed rounds, an alert-friendly health surface, and
  a dedicated storage-full counter. Note the recovery asymmetry worth a runbook line: on a 100%
  full target a round whose fold must write runs/seals dies BEFORE the pre-CAS delete phase, so
  GC may need externally-freed headroom before it can reclaim anything.
- [ ] **VERIFY: late-landing conditional PUT after fence loss** — a fenced mount never REPORTS
  success (`CasRequestControl.cpp:330-334` post-write fence check), but the physical PUT may have
  landed before the fence latched. Confirm the successor-side `writer_epoch` gating in
  `CasRefProtocol`/`CasRefLedger` rejects such a late ref-log object. This is the same hazard
  class as §1 "[Late Predecessor PUT]" and should be closed by rev.6 lease-boundary exclusivity —
  this audit re-flagged it from the backend side; fold the confirmation into the rev.6 work rather
  than tracking it separately.
- [ ] **MINOR: destructor-`abandon` live-epoch precommit debris** — if `abandon` fails while a
  failed transaction is being destroyed (e.g. the same backend outage that failed the commit), the
  LIVE-epoch precommit binding persists and is reclaimed only on REMOUNT — neither GC nor the
  (prior-epoch) stale-precommit sweep takes it (`ContentAddressedTransaction.cpp:117-123`, logged
  loudly). Bounded, but under a persistently broken backend it accumulates; consider a
  same-epoch periodic re-`abandon` retry or folding these into the mount-lease sweeper.
- [ ] **DOC: runbook notes from the audit** — (a) `CHECK TABLE` is the ONLY detector of silent
  same-length blob-body corruption (the CA layer never re-hashes payloads and never parses the
  envelope header on reads — sole `decodeEnvelopeHeader` caller is `CasInspect.cpp:491`); (b) a
  truncated blob surfaces as a premature-EOF read error via MergeTree size/checksum validation,
  not as a CAS-layer exception (`ReadBufferFromFileView.cpp:78-102` signals early EOF, no
  `physical size >= offset + length` check exists); (c) persistent target-full ends in a fenced
  mount + bounded-failing INSERTs — reads stay unaffected.

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

## [C2] defense-in-depth: fence-generation check on resurrectStaged + putOverwrite {#c2-resurrect-putoverwrite-fence-check}
**DONE (2026-07-23, whole-increment-review fix commit — I2).** Both condemned-displacement branches of `PartWriteTxn::uploadFromSource` now capture the mount fence generation at the displacement DECISION (`displace_admitted_generation = store->fenceGeneration()`) and re-check it via `store->checkFenceOrThrow` immediately before the raw `resurrectStaged` / `putOverwrite` call — the last two durable writes left outside Task 4's fence-generation gate are now covered. Scope held EXACTLY to these two calls; the debris deletes (`cleanupStagedManifestDebrisBestEffort`'s `deleteExact`, `cleanupPendingTempFiles`) were left untouched (proven structurally incarnation-safe). Tests: `CasFenceGeneration.CondemnedPutOverwriteAbortsWhenFenceTripsBeforeDurableCall` + `.CondemnedResurrectStagedAbortsWhenFenceTripsBeforeDurableCall` (RED-demo verified: without the check the displacement lands and the flow throws `NETWORK_ERROR`, not 668). Entry kept for history.

(2026-07-23, from rev.7 Task 4b review) The condemned-displacement branches of `PartWriteTxn::uploadFromSource` call `resurrectStaged`/`putOverwrite` raw — zero fence coupling. SAFETY already holds (the subsequent ref publish is fence-coupled → post-trip displaced blob = unreferenced debris; never-revive invariant intact — writer re-uploads its OWN bytes under a fresh incarnation_tag; putOverwrite is If-Match). Residual = one wasted post-trip write + a GC-liveness nuisance (fresh token dodges a queued exact-token condemned delete → one extra round). Adding `checkFenceOrThrow` there closes [C2] uniformly. SCOPE EXACTLY these two calls; do NOT chase the debris deletes (`cleanupStagedManifestDebrisBestEffort`'s `deleteExact`, `cleanupPendingTempFiles`) — both proven structurally incarnation-safe (build-scoped manifest keys / per-txn random staging keys).


## Operator assertion for natural Vanished(erased) + GC-quiescent wiring (land TOGETHER) {#erased-capability-operator-assertion}

> **OBSOLETE (2026-07-23): FORGET-only v1 decision** — the natural `Vanished(erased)` proof stack is excised entirely (spec rev.8 §9); no capability assertion is needed. Kept for history; the v2 door is the git history of the reviewed implementation.

Superseded 2026-07-23 (T17): the FORGET-only decision (the `OBSOLETE` note above) resolved the
formerly-pending question — the natural-`Vanished(erased)` proof stack (`gc_quiescent_fn`,
`setStrongPrefixListCapable`, the outstanding-request counter, the prefix-emptiness probe) was **excised**,
neither wired nor shipped dormant (spec rev.8 §9 excision list; plan excision task, commit `434f3214cec`).
Nothing remains to wire. Full technical detail is in the git history of the reviewed implementation (the v2 door).

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

## GC throughput collapse under a mass-DROP burst — RCA landed, three separable defects (2026-07-25) {#gc-throughput-collapse-2026-07-25}

Source: independent RCA by codex gpt-5.6 over the CA-s3 stateless lane log
(`tmp/gc-collapse-rca/`, prompt was facts-only after two of my own hypotheses were wrong and retracted).
Every load-bearing claim below was re-verified against the source by the controller.

**Observation.** On the CA-s3 lane (one server, CA is the default MergeTree disk, so every stateless
test drops tables into one shared pool) GC round wall time went `~20s → 72 → 98 → 195 → 532 → 1716s`
over rounds 27..32 while candidates went `188 → 20,046`. Round 33 was still in its pre-CAS path 39.5
minutes in when the capture ended. NOT a deadlock, NOT lease theft, NOT a lost `gc/state` CAS
(`grep -c "gc/state moved during the round"` = 0).

**Mechanism: a queue-stability crossing.** Rounds are serialized under `gc_round_mutex`, a round drains
ALL available work (no regular-round budget on ref-log, manifest-edge, candidate or owner-delete
counts), and dropped-namespace logs are protected from ref cleanup until a LATER completed round
observes physical emptiness. So DROP arrivals between completions (measured `97, 270, 250, 525, 1297,
4142`) feed the next round, and a longer round accumulates a bigger next batch. Once arrivals exceed
one serialized round's service rate the loop diverges. Self-inflicted, not external.

**Defect 1 (verified, cheap): the "bounded" meta pool has zero queue depth, so it back-pressures the
fold thread.** `Gc::Gc` builds `meta_pool` with the 4-arg `ThreadPool` ctor
(`CasGc.cpp:194`), which sets `queue_size = max_free_threads = max_threads = gc_meta_pool_size` (16)
— see `ThreadPool.cpp:161`. `scheduleMetaJob` uses `scheduleOrThrowOnError` (`CasGc.cpp:226`), i.e.
`wait_microseconds = nullopt`, i.e. `job_finished.wait(lock, pred)` — an UNBOUNDED block
(`ThreadPool.cpp:342`). So this is not an async fan-out: from the 17th in-flight condemn-marker write
onward the folding thread blocks on S3 latency in lockstep, and the fold's own LIST/GET work cannot
overlap with the condemn PUTs. Under the lane's saturated endpoint (50% of LISTs hit `Poco Timeout`
on attempt 1) 20k candidates at multi-second effective latency is tens of minutes — consistent with
round 33.
  - Bounding the round below is `meta_pool->wait()` before the single CAS (`CasGc.cpp:609`), so a
    deeper queue does NOT change the `candidates / pool_size × RTT` floor; it only buys overlap.
    Worth doing (a queue of, say, `4 × pool_size` keeps back-pressure and costs ~200 B per queued
    closure) but it is an amplifier fix, not the cure. MEASURE first — the endpoint was already
    saturated, so raising concurrency blindly can make it worse.
  - Note for S43: `CannotAllocateThreadFaultInjector::injectFault()` sits inside this exact
    `scheduleImpl`, and the CA fallback is "run the meta op inline" — correct, but it converts a
    thread-allocation failure into a further round slowdown. That is the behaviour S43 should assert.

**Defect 2 (verified, needs protocol work): completed namespaces keep permanent tombstones under the
globally-enumerated ref prefix.** Ref cleanup never deletes the `_cleanup` marker or the newest
constant-size `Removed` snapshot. Correct by design, but it makes the cost of every future fold grow
LINEARLY in the number of historically-unique namespaces, forever, with no self-limit under a workload
that keeps minting new ones (a CI lane; in production, every dropped table). The fix is to separate
completed-namespace tombstones from the prefix the fold enumerates — real protocol state and
migration, so: spec first, do NOT hack.

**Defect 3 (upstream, do not patch unilaterally): `system.remote_data_paths` has no `disk_name`
pushdown** (`StorageSystemRemoteDataPaths.cpp:153`, the TODO is already there), so
`04286_content_addressed_remote_data_paths` recursively LISTed the whole CA pool: 872 LISTs, 436
timing out on attempt 1. The server spent 1181.9 s and then returned `ABORTED` because the client had
already disconnected at the 600 s harness limit. This AMPLIFIED round 33 but did not start the
collapse — round 32 already took 28.6 min before that query began. Falls under the
upstream-consult-first rule.

**The `deleted` plateau (~700-744 while candidates hit 20,046) is a DIFFERENT mechanism, not a budget.**
`report.deleted` counts outcome-log entries of the delete pass (`CasGc.cpp:592`), and only
prior-pass `delete_pending` entries enter `merge.redelete` (`CasBlobInDegree.cpp:412`). So the pipeline
is condemn at round R → graduate at R+1 → delete at R+2, and each cohort lands two rounds later:
188→151, 571→535, 794→723, 1006→769. Round 32's 20,046 fresh candidates COULD NOT have contributed to
round 32's `deleted`. The 100-key `manifest_sweep_delete_budget_keys` applies only to the separate
orphan-manifest sweep and never populates this field. Same DROP burst drives both symptoms; the
plateau does not explain the long rounds.

**Cheapest thing that actually breaks the loop, in evidence order:** stop making CA-S3 the default disk
for the entire stateless lane (or throttle that lane's concurrency) — costs CA coverage breadth. The
robust fix is resumable/bounded regular rounds with durable progress plus defect 2's prefix split; a
NAIVE fold budget is unsafe, because an omitted `+1` edge must suppress deletion, not permit it.

**Measurement we do not have and need before choosing a production fix:** per-round wall-time split
across global ref LIST / ref-log+manifest GETs / candidate HEADs / meta-pool scheduling wait / blob+
manifest deletes / namespace+ref cleanup. Without it any knob choice is a guess.

## STUDY: GC bottleneck measurement rig + round introspection (opened 2026-07-25) {#gc-bottleneck-study-2026-07-25}

**Gates** every fix proposed in [#gc-throughput-collapse-2026-07-25](#gc-throughput-collapse-2026-07-25).
None of those may be implemented on the strength of the RCA alone: the RCA established the shape of the
feedback loop from log timestamps, but it did NOT establish where the time inside a round goes. Picking
a knob (meta-pool queue depth, `gc_meta_pool_size`, a round budget, a prefix split) without that split
is guessing, and two of the knobs can plausibly make things worse — the endpoint was already saturated.

### Why we cannot measure this today

The GC has **no timing instrumentation at all**: `grep -i "Stopwatch|elapsed"` over
`.../ContentAddressed/Gc/*.cpp` returns nothing. The one round-summary line
(`CasGcScheduler.cpp:276`) reports `candidates / deleted / absent / replaced / spared /
manifests_deleted` and **not even the round's own duration** — every wall-time number in the CI analysis
was reconstructed by subtracting consecutive log timestamps, which is why "where did round 33 spend 39
minutes" is unanswerable from the artifact. The existing `CasGc*` ProfileEvents
(`CasGcEnumerationPages`, `CasGcMetaOps`, `CasGcRetired*`, `CasGcClampSuppressedPasses`, …) are all
COUNTS of outcomes, never latencies and never per-phase request volume.

### Deliverable 1 — introspection (ship this first; it is useful in production, not just for the study)

Per-round, per-phase: wall time, S3 request count by verb (LIST/GET/HEAD/PUT/DELETE), bytes moved, page
count. Phases at minimum: global ref-prefix LIST; ref-log GETs; manifest-edge GETs; candidate HEADs;
meta-pool **scheduling wait** (distinct from meta-op execution — defect 1 predicts this is where the
fold thread parks); the `gc/state` CAS; owner-manifest deletes; namespace cleanup; ref cleanup.
Plus: retry/timeout counts per phase, so endpoint degradation is visible as itself rather than as
"GC got slow". Surface via ProfileEvents + the round summary line + `system.content_addressed_log`
(the B170 table already exists — see [[project_b170_cas_event_log]]). Decide deliberately whether the
per-phase split is always-on or behind a setting; always-on is preferable if the cost is a few counters.

### Deliverable 2 — a reproduction rig

The ca-soak driver is the wrong instrument: it is steady-state on a small table set, and this failure
mode is driven by **namespace churn**, not by data volume. The rig needs the CI lane's shape:
one server, CA as the default MergeTree disk, one shared pool, `gc_interval_sec=5`, a local S3
endpoint, and a generator that concurrently CREATEs and DROPs many short-lived tables. Target: drive
the pool to the CI numbers (≈15k namespaces, ≈9.5k dropped, ≈20k candidates in one round) in far less
than the 95 minutes the CI lane took, and make the arrival rate a knob so the queue-stability boundary
can be crossed on purpose and observed from both sides. It must also be able to hold the endpoint
UNsaturated, so endpoint slowness and GC cost can be separated — in the CI artifact they are confounded.

### Deliverable 3 — the measurements

1. Round wall-time split at a fixed pool size (answers "which phase dominates" — currently unknown).
2. Scaling curves, measured separately: cost vs number of live namespaces; vs number of *historically
   unique* namespaces (defect 2's permanent tombstones predict this one never flattens); vs candidate
   count. These three are conflated in the CI data.
3. The queue-stability boundary: DROP arrival rate at which round time stops converging, and how it
   moves with `gc_meta_pool_size` and with meta-pool queue depth. Defect 1 predicts a deeper queue buys
   overlap but does NOT move the `candidates / pool_size × RTT` floor, because `meta_pool->wait()`
   precedes the CAS — that is a falsifiable prediction, so test it.
4. Whether the collapse is self-limiting at some pool size or unbounded (open question from the RCA).

### Then, and only then

Choose a fix with numbers attached, and re-run the rig as the regression gate. Note that a naive fold
budget is unsafe by construction: an omitted `+1` edge must suppress deletion, never permit it — so any
bounded-round design has to carry durable progress state, which is why it needs a spec rather than a
patch.

## ROOT-CAUSED 2026-07-25, FIX OPEN: fsck vs GC disagree persistently about the same unreferenced blobs (soak v3) {#fsck-gc-indegree-disagreement-2026-07-25}

**LIVE REPRODUCTION EXISTS** on the ca-soak stand as left after soak v3 — do not `down -v` it without
capturing more. Artifacts: `tmp/f6-unreachable/{fsck_detail.txt,the_56_keys.txt,drain_watch.txt}`.

MEASURED on the idle stand, zero workload:
- `ca-fsck`: `reachable=406 dangling=0 unreachable=56 pending_gc=0 awaiting_gc=56 unaccounted=0`, FLAT
  across 8 samples over 5.3 min and again 8 min later. 56 blobs, 104,755 bytes.
- All 56 classify as `AwaitingGc` (`CasFsck.cpp:582`) with the note "edges still in the GC snapshot; the
  drop has not folded yet (expected)".
- GC holds the lease and folds once per 10 s (rounds 325..334+ observed). Every fold: `candidates=0`.
  `previewDeletes`: `dryrun_count=0`.

So fsck's reachability walk finds no live reference to these blobs, while GC's in-degree view assigns
them in-degree > 0 and never nominates them, and dozens of folds do not change either verdict. Exactly
one of the two components is wrong: fsck over-reports unreferenced, or GC under-collects (a retention
leak). WHICH IS NOT ESTABLISHED — do not accept a mechanism that has not been checked against the live
stand; two GC mechanisms proposed this week from log reading alone turned out wrong.

Note the fsck note's word "expected" is load-bearing and currently unearned: the state is only expected
if a later fold clears it, and folds did not.

Related: the ca-soak harness labels this population `(M-F debris, B140)` in `checker.py:544`,
`run.py:555` and the checkpoint lines. That label is wrong on its own terms — the product classifies
them as `AwaitingGc`, not as the abandoned-build/displaced-tree class the harness cites, and
`unaccounted=0` throughout. Fix the harness label together with whatever this turns out to be, not
before: the label change must not be what makes the number stop looking suspicious.

Sub-finding (cosmetic, fix with the round introspection in
[#gc-bottleneck-study-2026-07-25](#gc-bottleneck-study-2026-07-25)): the DEFER path returns at
`CasGc.cpp:368` before `report.round` is assigned, so every skip-unchanged round logs as
`CA GC round 0: candidates=0 …` and is indistinguishable from a round that folded and found nothing.
That cost real diagnosis time here.

### ROOT CAUSE (systematic debugging, 2026-07-25): an UNMATCHED-MINUS-ONE permanently retains one blob per occurrence {#unmatched-minus-one-retention-leak}

Resolves the investigation opened in
[#fsck-gc-indegree-disagreement-2026-07-25](#fsck-gc-indegree-disagreement-2026-07-25). Every step below
is a MEASUREMENT on the live post-soak stand or a line of source, not an inference.

1. **56 present blobs have no live reference.** `ca-fsck`'s reachability walk (refs -> manifests ->
   blobs) does not reach them. `dangling=0`, `unaccounted=0`.
2. **Each has exactly ONE residual source edge** in the adopted in-degree run
   (`soak_pool/gc/gen/343/attempt/1046/blob_target/0/0`, NDJSON): `{"b":"01<digest>","s":"<source_id>",
   "m":"edge"}`. Verified 56/56, one row each, 56 DISTINCT source ids. So most of each blob's edges were
   cancelled correctly and exactly one was not.
3. **An edge identity is a (manifest, file-path) pair**: `source_id = CityHash128(namespace ‖
   writer_epoch ‖ build_sequence ‖ manifest_ordinal ‖ path)` (`CasBlobInDegree.cpp:165`).
4. **The manifests that contributed those edges are GONE.** None of the 56 digests appears in ANY of the
   96 manifests still present in the pool. Methodology validated with a positive control: manifest bodies
   carry `"blob":"ch128:<hex>"`, and a reachable blob's digest IS found by the same grep.
5. **So GC computes in-degree = 1 and never nominates them**: `candidates_marked=0` on every one of 1062
   rounds, `ca-gc-dryrun` = 0, and the count is flat at 56 across 8 fsck samples over 5.3 min and again
   later — with zero workload on the CA pool (ca_stress has 0 active parts on BOTH nodes; the churning
   `system.*_log` tables live on the `default` disk, not on CA).
6. **This is NOT the missing-body path, and the designed protection never engaged.**
   `foldManifestEdges` returns false without emitting deltas when the body is absent
   (`CasGc.cpp:750-755`), and the caller is supposed to CLAMP the table (`CasGc.cpp:1123-1131`,
   "a missing manifest body is a per-table CLAMP (barrier), never a round abort"). Measured:
   `CasGcClampSuppressedPasses = 0` across 1062 rounds, against `CasRefManifestBodyFoldGets = 1,519,186`.
   The barrier never fired, so bodies WERE present at fold time and the `-1` WAS emitted.
7. **The `-1` therefore cancelled nothing.** The merge is a set-presence merge keyed by
   `(blob_ref, source_id)` (`CasBlobInDegree.cpp:585-597`): a remove delta whose key is not present in
   the prior is silently a no-op — `present` stays false, nothing is written, and **no counter records
   it**. The unmatched `+1` is carried forward from the prior run forever.

**CONSEQUENCE.** Every unmatched `-1` permanently retains one blob. The incremental GC can never reclaim
it — not a latency, a leak. Only `ca-gc-rebuild` (disaster recovery, full traversal) clears it. Observed
rate: 56 blobs / 104,755 bytes over one 4h soak. Unbounded in time.

**This also explains the CI observation** that the ca-soak harness has been labelling `(M-F debris,
B140)` for months. It is neither M-F debris nor B140.

**NOT ESTABLISHED — the next question, stated precisely:** why the removal computed a different edge
identity than the add. Two candidates: (a) the manifest body's entry paths differed between the
add-fold and the remove-fold, so the `-1` was keyed on a different `path`; (b) the removal named a
different `ManifestRef` (epoch/build/ordinal) than the add. Distinguishing them requires the MANIFEST
ID on the edge events — `system.content_addressed_log` currently records only namespace + object_hash
for `root_add`/`root_remove`, which is exactly why this could not be closed from the event log. Concrete
introspection requirement for [#gc-bottleneck-study-2026-07-25](#gc-bottleneck-study-2026-07-25).

**CHEAP AND SAFE NOW (does not fix the leak, makes it visible):** count remove deltas that matched no
prior edge, as a ProfileEvent + a WARNING with the blob and source id. Pure observability, no protocol
change, no behaviour change. A silent no-op on an unmatched cancel is the reason this survived months of
soaks. Do this even before the mechanism is known.

**DO NOT** "fix" it by making the fold drop edges whose manifest is absent: an omitted `+1` must suppress
deletion, never permit it, and that change would delete live data on any transient body read failure.

Artifacts: `tmp/f6-unreachable/` — `fsck_detail.txt`, `the_56_keys.txt`, `drain_watch.txt`,
`run343.bin` (the decoded in-degree run), `manifest_bodies.txt` (all 96 present manifests).

#### Event-log forensics (2026-07-25): all 56 leaks trace to FOUR `tmp-fetch_*` refs in a 43 ms window {#unmatched-minus-one-fetch-window}

Follows [#unmatched-minus-one-retention-leak](#unmatched-minus-one-retention-leak) and CORRECTS one of
its conclusions.

**Correction 1 — the event log DOES carry the edge identity.** I wrote that it records only namespace +
object_hash. Wrong: `root_add`/`root_remove` rows carry `detail['manifest_ref_instance']`
(`writer_epoch:build_sequence:manifest_ordinal`) and `detail['path']`, which together with `namespace`
are exactly the inputs of `sourceEdgeId`. The edge balance IS reconstructable from the log.

**Methodological trap worth remembering: you must UNION both nodes' logs.** Edge events are emitted by
the folding leader, leadership moved between ch1 and ch2, and each server has its own
`system.content_addressed_log`. Querying ch1 alone showed ~19 "uncancelled" edges for a single blob;
after unioning ch2 it collapsed to exactly ONE — matching the in-degree run's ground truth exactly.

**Correction 2 — the `-1` was not "emitted with a mismatched key"; no `root_remove` was ever recorded
for these edges at all.** Per-path balance for manifest `1:42969:1` (ns `ca_soak_ch1`) is `adds=1
removes=0` for ALL TWENTY of its paths, so the whole manifest's removal never folded — not a per-path
key mismatch.

**Where they come from.** All 56 blobs' uncancelled edges belong to just FOUR manifests, all in
`ca_soak_ch1`, build_sequences 42969/42970/42971/42972, and every one of them is a FETCH TEMP ref:
`tmp-fetch_20260725_2680{1,2}_…`, `tmp-fetch_20260725_2679{8,9}_…`. All four were published
(`build_publish` → `promoted`) between 08:37:56.924 and .945 and dropped (`ref_drop`, "appended an
owner_transition removal ref-log transaction") between .960 and .967 — a 43 ms window. Each published
exactly once and dropped exactly once.

**Rate: 4 out of 48,791 `tmp-fetch_*` refs in that namespace (~0.008%)** — a rare race, not a broken
path, and clustered in one 43 ms window with four concurrent fetch publishes.

**The ordering anomaly.** Their `+1` edges were folded at 08:40:51 — about three minutes AFTER the
`ref_drop` that should have cancelled them.

**LEADING HYPOTHESIS, NOT VERIFIED: an ordering inversion.** If the fold applied the removal record
BEFORE the add record (journal position, not wall-clock), the removal would find no edge and be a
SILENT no-op (the set-presence merge, `CasBlobInDegree.cpp:585-597`, writes nothing and counts nothing),
and the subsequent add would then install an edge that nothing will ever cancel. That is precisely an
[UNMATCHED-MINUS-ONE]. Verifying it needs the ref-log record order for those transactions, and those
log objects have long been reclaimed on this stand — so it needs a TARGETED REPRODUCTION (concurrent
fetch publish + immediate drop), not more archaeology here. Note this sits squarely in the fetch-handoff
area the publish-confirm plan already covers.

**Caveat on the numbers.** 19 chaos container restarts × a 2 s `flush_interval_milliseconds` means the
event log can lose buffered rows. Log-derived uncancelled edges came to 80 against the run's ground
truth of 56, so some rows ARE missing. The per-manifest conclusion is still safe: a manifest's `+1` and
`-1` are emitted microseconds apart inside one fold, so losing the `-1` while keeping all twenty `+1`s
is not a plausible loss pattern.

## *** CRITICAL / RELEASE-BLOCKER: GC treats a paginated `LIST` as the journal, so a single incomplete page can silently DELETE LIVE DATA (2026-07-25) *** {#list-as-journal-dataloss-2026-07-25}

Independent RCA by codex gpt-5.6-sol (xhigh) on the facts-only package in `tmp/leak-rca/`, dispatched
after the retention leak in [#unmatched-minus-one-retention-leak](#unmatched-minus-one-retention-leak).
It REFUTED my ordering-inversion hypothesis and found a worse defect. Full output:
`tmp/leak-rca/codex_rca.log`. The load-bearing step was re-verified by the controller (see below).

**The defect.** GC discovers ref-log transactions by paginating `LIST(cas/refs/)`, groups the returned
keys per namespace, and folds those above the namespace cursor (`CasGc.cpp:829`, `:1033`). The listing
API gives a continuation cursor but NO snapshot token, high-water mark, or contiguity proof
(`CasBackend.h:359`, `CasObjectStorageBackend.cpp:1083`), and transaction ids are not required to be
contiguous — there is no gap check anywhere in the fold. The cursor then advances "per FULLY folded
log", i.e. per record the round happened to SEE. A record omitted from one page is therefore skipped
permanently: on later rounds it sorts at or below the cursor and is ignored, ref-log cleanup may delete
it (`CasGc.cpp:1502` requires cursor coverage, not proof the record was applied), and the orphan sweep
may then reclaim the manifest body (`CasOrphanManifestSweep.cpp:197`).

CONTROLLER VERIFICATION of the two load-bearing claims: the comment at `CasGc.cpp:1035-1037` does say
"the durable cursor advances per FULLY folded log", and `grep -niE "contiguous|gap|prev_txn"` over
`CasGc.cpp` returns nothing that checks for a hole. Both hold.

**BLAST RADIUS — this is a data-loss class, not a leak class.** The listing/cursor logic is
operation-agnostic, so it can drop a `+1` exactly as easily as a `-1`:
1. manifest `M1` owns blob token `B`; edge `E1` is known to GC.
2. a new live manifest `M2` adopts the SAME deduplicated token `B`.
3. `M2`'s `+1` is omitted from a listing page while GC advances past it.
4. `M1` is removed and its `-1` folds normally.
5. GC now sees zero edges for `B`, condemns it, graduates it, and deletes it by exact token —
   while `M2` still references it.
Exact-token deletion does NOT protect against this: `M2` references that exact token. Condemnation
protects writes that occur AFTER condemnation; it cannot protect an already-committed owner whose `+1`
was skipped. Dedup is what turns a silently-skipped `+1` into a deletion, which is why the observed
symptom so far has only been retention.

**What the observed 56 blobs are:** the benign polarity of the same defect. And per codex the rate is
NOT 4-in-48,791 independent failures — the removals were batched (two refs share one `at_version`), so
it is closer to ONE holey scan affecting four refs.

**Origin of the holey page is NOT established** and is not recoverable from the captured state.
Candidates: rustfs returning an incomplete/inconsistent page; S3 continuation behaviour under
concurrent mutation; a bug in or below the iterator. Note ordinary concurrent append during a CORRECT
paginated scan is NOT sufficient — a later same-namespace record was returned while an already-durable
earlier one was not, which requires a real hole in the return set.

**Fix plan (codex's, ordered).**
0. **Containment:** disable destructive GC for the current pool format — blob graduation/deletion,
   ref-log cleanup, orphan-manifest cleanup. Folding and diagnostics continue. A pool whose journal
   coverage cannot be proven must not delete.
1. **Replace LIST-as-journal with an authoritative per-namespace chain:** immutable `prev_txn_id` on
   each transaction; an exact-addressed CAS-updated namespace head whose update IS the commit point;
   an add-only exact-addressed namespace registry (itself chained — discovering namespaces by `LIST`
   would recreate the defect). Format bump, no dual protocol (pre-release).
2. **Make destructive GC conditional on a complete cut:** capture registry cut + per-namespace heads at
   round start, traverse exact predecessor links back to the sealed cursor, require an exact meet; on
   any unreadable/unvalidatable link, discard that namespace's fold and GLOBALLY suppress destructive
   actions for the round (an unproven namespace may reference any blob). `LIST` becomes cleanup
   inventory only, never evidence of completeness.
3. **Ordering + diagnostics:** carry `(txn_id, op_ordinal)` into `BlobDelta` and validate by explicit
   journal order; COUNT unmatched `-1`s, head-chain gaps, cursor/head lag, and cleanup attempts on
   unproven records; preserve suspect logs on any anomaly instead of cleaning them.
4. **Recovery:** format bump + fresh pools; for the affected pool keep destructive GC off or recreate
   from authoritative replicas. NOTE `ca-gc-rebuild` is NOT a sufficient release fix — it also
   discovers ref state through listing.

**Reproduction (primary regression test):** subclass `CasInMemoryBackend`, seed `A` (owner-add),
`R` (its owner-remove), `H` (later harmless record); have the first ref-prefix `LIST` return `A` and `H`
but filter out `R`, while exact `GET(R)` still works. Assert today: cursor advances to `H`, a residual
`+1` survives, restoring the listing does not cancel it. Add the MIRROR SAFETY TEST: omit a second live
owner's `+1`, fold the first owner's `-1`, drive graduation, and assert the live blob is never deleted.

**Codex's corrections to my evidence, accepted:** M5's event-log argument is supportive, not conclusive
(add and remove may fold in different rounds, so a restart can keep one batch and lose the other) — the
source-edge run plus the durable `ref_drop` plus the reclaimed removal logs is the stronger argument.
M3 proves no OBSERVED transaction hit an absent body, not that every transaction was observed — which is
exactly why the clamp never engaged. M4's silent no-op is real but probably not exercised here: the
evidence supports "the remove never reached the reducer", not "it folded first". M8 is fold lag, not
journal inversion.

**Relationship to the publish-confirm plan:** Task 1 already pins `[UNMATCHED-MINUS-ONE]`, but only in
the no-premature-deletion direction (see its own test comment). The retention direction is unpinned, and
this entry shows the deletion direction is reachable by a DIFFERENT route than the counter-regression
Task 1 guards. Tasks 13-14 (wire protocol, receiver flow) rewrite the very `tmp-fetch` lifecycle where
this surfaced — sequence them AFTER containment, or attribution of future findings will be hopeless.

## The adopted fold seal referenced a PRUNED generation's run (observed 2026-07-25, same stand) {#adopted-seal-pruned-run-2026-07-25}

Noticed while verifying an unrelated change; recording the measurement before it is lost.

MEASURED on the post-soak stand, one consistent read:
- `gc/state` = `{"round":342,"gc_shards":1,"snap_generation":342,"snap_pruned_through":339,"snap_attempt":1043,...}`
- the ADOPTED seal `soak_pool/gc/gen/342/attempt/1043/fold_seal` carried
  `"blob_target_runs":[{"key":"soak_pool/gc/gen/339/attempt/1016/blob_target/0/0","generation":339}]`
  (reference-parent carry — a current shard's run legitimately living under an older generation's key)
- that run object **did not exist**: `ca-inspect` reported "key … does not exist", and an `s3` listing of
  `soak_pool/gc/gen/**` returned only the seals for 341/342/343 plus gen 343's own run.

That is exactly what `CasGc.cpp:629-645` says must never happen. `pruneSupersededGenerations` is passed a
`referenced_generations` set built from the new seal's runs AND the parent seal's runs, with the comment
"Retention must never reclaim these" and "a losing leader must not destroy what the winning leader's
already-adopted seal still points at (triage #5)". Here the adopted seal's referenced generation was
reclaimed anyway.

CONSEQUENCE while it lasts: `zeroInDegree` iterates the adopted seal's runs, so an absent run yields ZERO
candidates — GC keeps completing rounds and reclaims nothing, silently (per
[[feedback_ca_gc_never_throw_on_404]] GC must not throw on a 404 during fold, so nothing surfaces). This is
NOT the explanation for the 56 retained blobs — a later observation had gen 343 adopted WITH its run present
and `candidates_marked` still 0 — but it is an independent defect and a second reason a round can quietly
collect nothing.

NOT ESTABLISHED: whether the prune raced the adopt, whether `snap_pruned_through=339` is inclusive of 339,
or whether the parent-seal capture missed the carry. Needs the same treatment as the other GC findings: a
targeted test, not log archaeology. Note the stand is still up.

## FIXED 2026-07-25 (`ca5a6b7bee8`): THIRD gtest gate-filter gap — parameterized `*/CasBackendContract` suites match neither `Cas*` nor `CA*` {#gate-filter-gap-3-backend-contract}

Found by ENUMERATING the binary's suites instead of trusting the documented filter — which is the only
method that works here, two previous gaps having been found the hard way (see
[[reference_ca_gtest_gate_filter]] and {#gate-filter-gap} above).

`InMemory/CasBackendContract` and `Local/CasBackendContract` are value-parameterized, so their full test
names begin with the instantiation prefix (`InMemory/CasBackendContract.X/0`). A `Cas*` or `CA*` pattern
anchors at the start of the FULL name, so both suites — the backend contract itself, i.e. exactly the
layer every GC and ref-lane conclusion rests on — were excluded from every battery run.

CORRECTED FILTER, verified by enumeration against `--gtest_list_tests` on 2026-07-25:

```
Ca*:CA*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:*CasBackendContract*
```

`Ca*` subsumes `Cas*` and also picks up `CaLifecycle`/`CaWiring` (gap 2). The trailing `*CasBackendContract*`
is unanchored on purpose — that is the whole point. Runs 1335 tests over 227 suites; green as of
`84cefb2c224`.

Note `Cas*` also sweeps in `CascadeWriteBuffer`, which is unrelated to CAS and merely shares a prefix.
Harmless (it passes), but do not read the suite count as "all CAS".

LESSON, worth generalizing: a name-pattern gate silently under-tests and never reports what it skipped.
Any gate defined by a glob should be re-derived from an enumeration periodically, not maintained by hand.

### UPGRADE 2026-07-25: the deletion path is no longer an inference — it is MECHANISED in TLA+ {#list-as-journal-mechanised}

`docs/superpowers/models/CaRelinkConfirmCore.tla`, config `_sab_holeylist` (Task 9 of the publish-confirm
plan, landed `0d1e3f4cc7c`). Controller re-ran it independently: `_main` completes clean at 72,984 states,
`_sab_holeylist` violates.

The sabotage config changes NOTHING about the confirm protocol — every rule stays intact — and permits
exactly **one** incomplete `LIST` page in the whole behaviour (`MaxHoles = 1`).
`ConfirmedRelinkNeverDangles` breaks anyway. Essential trace: the receiver's `+1` is durable; an
edge-neutral later transaction lands in the same namespace; confirm CORRECTLY answers yes; promote; the
single holey fold observes the later transaction but omits the `+1` and advances the namespace cursor past
it; the record is below the cursor forever, so complete later pages cannot recover it; condemn →
`delete_pending` → delete over three rounds; a promoted, correctly-confirmed manifest now references a
deleted blob.

Two details that make this stronger, not weaker:
- The one-hole budget was TIGHTENED during modelling. A first version let GC be arbitrarily lazy and TLC
  found a cheap violation with no permanent skip at all; the adversary was constrained so the counterexample
  is FORCED through the permanent skip. The finding survived being made harder to reach.
- Three-phase graduation's sparing does not help. It can only spare what a fold shows it.

CONSEQUENCE for how the model may be cited: `_main` passing is CONDITIONAL on a LIST-completeness
assumption the shipped code does not establish. It means "the confirm protocol adds no new dangle path",
NOT "a confirmed relink cannot dangle". It may not be cited as dangle-freedom until fix items 1-2 (the
authoritative per-namespace chain and the complete-cut gate) land.

### CORRECTION 2026-07-25: the "live reproduction" stand was destroyed by our own S42 smoke runs {#leak-repro-lost}

The stand referenced by [#unmatched-minus-one-retention-leak](#unmatched-minus-one-retention-leak) as a
live reproduction STOPPED being one at ~16:49 UTC, and the worklog kept asserting otherwise for another
hour. Cause: the S42 card's smoke runs reset the cluster (two runs, `20260725T164254` and
`20260725T164929`), which recreated the pool. Found only because the state was re-checked before teardown
instead of trusting the note — post-teardown check read `reachable=6 unreachable=0`, i.e. a fresh pool.

No evidence was lost: every artifact was captured between 12:50 and 15:00 and is in `tmp/f6-unreachable/`
(detail fsck, the 56 keys, the decoded in-degree run, all 96 manifest bodies, the uncancelled-edge table,
the per-path balance for the leaking manifest, both nodes' event-log slices). The mechanism is also
mechanised in `CaRelinkConfirmCore.tla` `_sab_holeylist`, and the fix needs a targeted reproduction rather
than this stand.

LESSON, and the reason this is recorded rather than quietly dropped: a scenario card that resets the
cluster is indistinguishable, from the outside, from one that does not. If a stand is being held as
evidence, that has to be enforced (a lock file the cards honour, or a separate compose project), not
merely written in a log — the same "a note is not a mechanism" failure as the harness whitelists found
today.

### DECISION 2026-07-25 (user), DETECTOR SHIPPED 2026-07-26: option C — catch it by the tail first, because the diagnosis is NOT confirmed {#list-as-journal-decision-c}

Chosen over containment (disable destructive GC now) and over going straight to the journal chain.
Reasoning recorded because the reasoning is the load-bearing part:

- The pool is PRE-RELEASE with no production data at risk, so the present cost of the hole is possible
  data loss in our OWN test pools — bad for attributing soak failures, not a loss of anything valuable.
- Containment would disable GC exactly while we are building a soak gate to exercise GC, would break the
  soak's disk budget (the last run reclaimed 124 GB at a single checkpoint), and would MASK the other GC
  defects already in this backlog.
- The chain + complete-cut fix is right but is protocol state plus a format bump, and it would cut across
  Part B, which is mid-flight.

**The user's qualification, which changes the design and not merely the wording: the LIST hypothesis is
still UNCONFIRMED.** A holey page was never directly observed; it survives by elimination and is
mechanised in TLA+, which proves the mechanism is SUFFICIENT, not that it is what happened.

CONSEQUENCE — the detector must not be built for that hypothesis. A LIST-hole detector catches only a
LIST hole; if the record is instead lost further down the pipeline (delta routing across gc shards, the
reducer, the run flush) such a detector stays silent AND manufactures false confidence. So detect the
EFFECT, mechanism-agnostically: *the fold cursor advanced past a transaction that was never applied.*

Two cheap, complementary probes, neither assuming a cause:
1. **The store lied** — after paginating the ref prefix and BEFORE advancing any cursor, re-derive the id
   set independently and require agreement. A mismatch means the object store gave two different answers
   about the same durable prefix; suppress destructive actions for the round and log loudly.
2. **We dropped it** — count records intended-to-fold versus actually-applied per round and require
   equality. This is the half a LIST-focused detector would miss entirely.

Plus the mirror SAFETY test from the RCA, which is valuable independent of any of this: omit a second
live owner's `+1`, fold the first owner's `-1`, drive graduation, and assert the live blob is never
deleted. That pins "a missing `+1` suppresses deletion" BEFORE the protocol gets rewritten, not after.

NOTE the detector is not free to design: id gaps are LEGITIMATE — Task 18 (landed today, `252ccbdf2d4`)
deliberately leaves a safe gap when an append is refused before any attempt. So "a hole in the sequence"
is not by itself an anomaly. An object that EXISTS in the store at or below the cursor and was never
applied is unambiguous; a gap with no object is not.

COMMITMENT attached to this choice, stated by the controller and accepted implicitly by choosing C: the
real fix (the authoritative per-namespace chain and the complete-cut gate) lands BEFORE release. C makes
the defect visible; it does not prevent it.

## USER DECISIONS 2026-07-25 on the four open questions {#user-decisions-2026-07-25}

### Q2 — the tool-vs-live-pool problem: the FRAMING was wrong, not just the answer {#q2-force-claim}

Both my question and the design note
(`specs/2026-07-25-cas-tool-read-without-ownership-design.md`) treated this as "may a tool read without
claiming the mount". **It is not that.** The user: the problem is the differing SERVER UUID, not
`mountWritable`. What is needed is the ability to say at mount time *"never mind that the server uuid
differs — force a new one"* and mount as WRITE. A genuine read-only mount is a SEPARATE, not-yet-implemented
task, not the answer to this one.

So the design note is superseded as a recommendation. Its VERIFIED facts stand and stay useful — in
particular that a claim against an owner-absent empty root SUCCEEDS and would lock the real server out
(`CasServerRoot.cpp:142` then `:120-131`), and that the earlier CI carve-out never actually ran because its
`sed` patches `config.xml` while the CA disk is declared in `config.d`.

ONE FACTUAL CONCERN to settle before implementing, raised once and then dropped: the plan text describes
the scrape as running against the data directory of a LIVE server. Force-claiming there does not merely
bypass a nuisance check — it takes ownership from a running server, which is the failure the refusal
exists to prevent. If the scrape actually runs post-mortem (server already stopped), force is exactly
right and this concern is void. Worth one look at the CI step before coding, not a redesign.

### Q3 — `ca-fsck` stays NON-fatal on `stale_edge` for now {#q3-stale-edge-nonfatal}

As implemented. No change. (The controller's suggestion to bind the flip to the leak fix was not adopted;
leave it simply non-fatal.)

### Q4 — what "green" means for S42 {#q4-s42-green}

Green is **a consistent state on disk and in memory**, NOT "we proved a fault landed in the post-durable
install window". This overrides the card's current soundness guard, which returns `inconclusive` whenever
the targeted poison/failpoint signal is zero — a signal that is zero by construction today, since the
install-region seam is gtest-only.

Consequence for the card: the verdict rests on the consistency assertions (post-restart view identical to
pre-restart, journal-rebuilt view containing every acked block, replicas agreeing, fsck clean). The
targeted counters stay REPORTED, not gating. Keep an anti-vacuity guard on the GENERIC fault count so a run
in which no allocation fault occurred at all still cannot read as green — that part of the discipline
survives; it is only the window-specific targeting that is dropped.

### DONE 2026-07-26 — Q5 option (c): introspection now, study later {#q5-gc-introspection-now}

Do the introspection NOW. Shape specified by the user: **each GC phase emits its own ROW in
`system.content_addressed_garbage_collection_log`**, carrying all the metrics that matter for that phase —
not extra columns on the single per-round row.

Then, AFTER Part B and its soak: study the collected metrics, look for anomalies, and only then decide what
further investigation is warranted. The reproduction rig from
{#gc-bottleneck-study-2026-07-25} is therefore NOT built yet — real metrics from real runs come first and
may well retarget it.

## Operator recovery: mounting a pool whose owner uuid differs (deferred 2026-07-25) {#operator-uuid-recovery}

Split out of {#q2-force-claim} once the CI motivation for it evaporated. The CI scrape is fixed by a
one-line change to which file its `sed` patches — the read-only path already exists in the product and CI
already tried to use it — so nothing about the scrape needs a product change. See
{#ci-scrape-readonly-sed-fix} below.

WHAT REMAINS, and it is a real operator need, not a CI one: a server whose local uuid file was regenerated
(wiped `/var/lib/clickhouse`, a pod recreated without a persistent volume) cannot mount its own pool. The
refusal at `CasServerRoot.cpp:120-131` already names the three manual recoveries — restore the uuid file,
configure a fresh `server_root_id`, or delete the owner object by hand after verifying no server uses the
root. A supported command would automate the third.

TWO READINGS, and the choice matters — settle it before implementing:
- **Overwrite the owner uuid with a new one** (the literal reading of "force a new one"). Works, and
  permanently locks the ORIGINAL server out of the pool. Also insufficient on its own: the uuid lives in
  two durable objects, and a graceful shutdown leaves a mount object carrying the predecessor's uuid, so
  `claimMount` then refuses it as `ForeignOwner`. The force has to cover both, in an order that keeps the
  refuse-to-re-mint-epoch-1 guard armed.
- **Adopt the pool's existing owner uuid and mount as it.** Reaches the same "mount as WRITE despite a
  differing local uuid" outcome with no durable identity damage. `Pool::openForDecommission`
  (`CasPool.cpp:720-776`) already does exactly this, so most of the work exists.

The second reading looks strictly better for the stated need and the first should have to justify itself.
Not decided; not started.

NOTE this is NOT the read-only-mount task. The user was explicit that a genuine read-only mount is a
separate, unimplemented piece of work, and it — not this — is the right answer to "an operator wants to
look at a live pool from a second process".

## CI scrape opens CA disks read-only — the remedy existed but never applied (fixed 2026-07-25) {#ci-scrape-readonly-sed-fix}

`dump_system_tables` already carried the correct remedy: insert `<readonly>true</readonly>` next to the
`content_addressed` marker so `clickhouse local` skips `mountWritable` and never claims ownership. It
patched `/etc/clickhouse-server/config.xml`, where that marker does not exist — the CA storage policy is
symlinked into `config.d/` by `tests/config/install.sh`. So `sed` matched nothing, silently, and the
scrape kept dying on "owned by a different server" while looking like it had been handled.

Fixed by patching `config.d/*.xml` as well, and by making a future no-op LOUD: if a CA disk is declared
and the read-only marker is absent afterwards, the job now prints a warning naming the consequence. A
silent no-op is how this survived in the first place, and it is the same shape as three other harness
surfaces found the same day ({#gc-observation-vacuous-2026-07-25} and the entries it references).

Verified by simulating the substitution against the two real config files before committing, rather than
by reading the sed and believing it.

## Part B codex review — DO NOT MERGE AS-IS: two blockers, two majors (2026-07-25) {#partb-review-findings}

Full output `tmp/partb-review/codex_review.log`, package `tmp/partb-review/`. gpt-5.6-sol at xhigh over the
combined 23-file diff, read as ONE protocol — which is exactly what none of the eight per-task agents did.
The normal gate-0 → gate-1 → prepare → confirm → promote ordering is confirmed sound, and tasks 13-16 did
NOT make `No` authoritative. Everything below is what composition exposed.

### BLOCKER 1 — a completed recovery can expose a stale row and authorize `Yes`

Recovery discovers snapshots and logs through paginated `backend.list` with no completeness proof, then
publishes the result as `recovered = true` (`CasRefLedger.cpp:463`, `:736`). `confirmExactRef` treats a
recovered, quiescent, clean, fenced runtime holding the exact row as sufficient for `Yes`
(`CasRefLedger.cpp:334`). Atomic publication prevents a HALF-BUILT view; it does not prove the listing was
COMPLETE.

So if recovery's listing omits a later durable removal or repoint, it publishes a stale but
apparently-healthy runtime and gate 1 answers `Yes` for a manifest that is gone. **That is a route around
every ambiguity check** — the ambiguity was erased when incomplete recovery set `recovered = true`. If GC
already folded the omitted removal and collected the blobs, the receiver commits a dangling manifest.

This is DISTINCT from `_sab_holeylist`, where a CORRECT `Yes` still dangles because GC skips the receiver's
own `+1`. Same root cause (LIST as journal), different path.

Short-term safe behaviour: a LIST-recovered runtime must be ineligible for `Yes`. Real fix is the
authoritative head/predecessor traversal already recorded in {#list-as-journal-dataloss-2026-07-25}.

### BLOCKER 2 — an uncertain `precommitAdd` loses its cleanup owner and may delete its manifest body

`PartWriteTxn::precommitAdd` records `precommit_target_ns` / `precommit_manifest` / `precommitted = true`
only AFTER `appendRefOps` returns successfully (`CasPartWriteTxn.cpp:1013`). But an `Unresolved` append MAY
HAVE LANDED — the code says so itself (`CasRefLedger.cpp:2152`). So on that path `prepareEntries`' catch
calls `abandon` on an object that believes it never precommitted (`PartFolderAccess.cpp:450`): no removal
is queued, no handle is returned, the error is converted to `MechanismFallbackAllowed`, and — worse —
`abandon` best-effort DELETES the manifest body (`CasPartWriteTxn.cpp:1394`). When the wedge later resolves
as committed, it installs a live precommit with no owning handle and possibly no body.

This falsifies taxonomy row 2's "never staged". Needs an explicit uncertain-precommit state whose cleanup
ownership survives the call; an arbitrary `NETWORK_ERROR` cannot be classified as safe byte fallback.

### MAJOR 3 — a promote can commit and still be reported as fallback or failure

`promote` maps every `NETWORK_ERROR` to `MechanismFallbackAllowed`
(`ContentAddressedMetadataStorage.cpp:2057`), but the promotion PUT may have landed. There is also a purely
local post-commit window: `promoteBuild` builds a `CommitOutcome` with copied strings BEFORE marking the
handle terminal (`PartFolderAccess.cpp:325`), so an allocation failure there enters the failed-promote
catch with the ref already committed. One logical fetch can then durably publish the relink ref, report
fallback, remove it, and publish the byte-fetched ref — a sequential double publication rows 5 and 6 claim
cannot happen. Promotion needs a terminal outcome distinguishing not-committed / committed / unresolved,
and nothing after a durable commit may throw before the handle records `Committed`.

### MAJOR 4 (latent) — move assignment can discard a terminal duty

`PreparedPartWrite::operator=` overwrites the destination's build even when `abandonBuildBestEffort`
returns false (`PartFolderAccess.cpp:380`), permanently dropping a cleanup owner. Not exercised today (the
exchange path uses move construction), but the move-only ownership contract is false as written. Delete
move assignment or preserve the original handle on failed cleanup.

### Comments that overclaim, and one that is simply wrong

- `CasRefLedger.h:57` states "`No` is a proof of the negative". It is not — the fence is evaluated LAST, so
  a fence-lost mount answers `No`. This is the exact inversion the whole round has been guarding against,
  sitting in the header that defines the contract.
- `ContentAddressedMetadataStorage.cpp:2128` and `DataPartsExchange.cpp:1446` both claim every subsequent
  GC fold sees the receiver's `+1`. The taxonomy caveat at `DataPartsExchange.cpp:1365` correctly
  contradicts them.

### Footprint objections (to weigh, not all accepted)

`DataPartsExchange.h` exposes CAS-specific `allow_ca_relink` and service helpers beyond the opaque enum;
receiver work spans `fetchSelectedPart` and `relinkPartToDisk`, sender work spans `processQuery` and two
helpers. Both were knowingly accepted at the time and reported to the user. The reviewer also counts the
`ProfileEvents.cpp` registration as outside the approved failpoint scope — that is a conflation: it came
from Task 18, not the failpoint approval, and a ProfileEvent registration is not protocol coupling. Worth
the user's judgement, not a silent dismissal.

### Test gaps, and one pre-existing test that is green for the wrong reason

`test_replicated_fetch_by_relink` (`test.py:255`) still proves relink ONLY through a flat blob count — the
exact defect Task 16 found in the plan, left unfixed in the older test. A byte fetch dedups and passes it.

Missing: recovery after one omitted LIST record then `confirmExactRef`; `LandedThenLost` during receiver
`precommitAdd`, promotion and abort; allocation failure after durable promotion; move assignment with a
failing abort; every gate-0 case (the gtest says integration owns them and no integration test does);
source remount between offer and confirm; zero/multiple routing matches; confirm transport failure at HTTP
level.

### CORRECTION 2026-07-26 (user): "BLOCKER 1" is NOT a Part B defect — the confirm inherits the mount's trust, it does not create it {#partb-review-blocker1-downgraded}

Downgraded from {#partb-review-findings}'s blocker 1 after the user challenged the framing. The challenge
was right and the controller had accepted the reviewer's attribution without checking the boundary.

VERIFIED: `confirmExactRef` reads the same `RefTableRuntime` and the same recovered `state` that
`resolveRef` reads (`CasRefLedger.cpp:182` uses `getRefTableRuntime` + `ensureRefTableRecovered`;
`confirmExactRef` at `:318` uses `find` on the same map purely to avoid CREATING a runtime). It is the same
in-memory view that serves every ordinary read and that the write path makes its decisions on.

So the confirm introduces no new trust assumption — it inherits the mount's. If that view can be stale in a
way that matters, then the sender is ALSO serving reads from a manifest that is gone and deciding writes on
a false picture. The blast radius is the mount, not the confirm, and the remedy is not in the confirm
protocol.

Blocker 1 is therefore the already-recorded {#list-as-journal-dataloss-2026-07-25} finding seen through one
more lens, not a defect introduced by Part B. The reviewer stated a true fact and attributed it to the
wrong component.

WHAT SURVIVES, and it is smaller than the heading suggested: the confirm gives a stale LOCAL view a REMOTE
consequence — without it a corrupt view harms its own mount, with it a peer can durably commit on the
strength of it. That is amplification, not a new root cause, and it does not change the remedy.

CONSEQUENCE the controller must own: the argument for moving the journal-chain fix AHEAD of the soak was
built on "the same root now reaches a second path". That argument is weakened — same root, same path, plus
propagation. The scheduling decision should rest on the original grounds, not on a blocker that turned out
not to be one.

The other three review findings (uncertain `precommitAdd`, promote-committed-reported-as-fallback, move
assignment) are unaffected and are being fixed.

### Part B review findings — RESOLVED, with two reviewer errors and one residual window (2026-07-26) {#partb-review-resolved}

Fixed in `8e6fe6ef0af`. Gate 1373/1373 (five new tests, each seen red first), integration 11/11.

Blocker 2, major 3 and major 4 are closed. Blocker 1 was downgraded, not fixed — see
{#partb-review-blocker1-downgraded}; it is the known LIST-as-journal finding, not a Part B defect.

**TWO PLACES THE REVIEWER WAS WRONG**, both caught by the implementer rather than by me — I had passed the
first one straight through into the fix instructions:

1. "Queue the exact removal (it is idempotent)" is FALSE. `RefTableState::applyOwnerTransition`'s
   `RemovePrecommit` arm throws `CORRUPTED_DATA` on an absent binding
   (`CasRefProtocol.cpp:216`). Following the review literally would have made every `abandon` of an
   uncertain build fail forever, destructor retries included — turning a leak into a permanent wedge. The
   removal is presence-checked under `Uncertain` only.
2. Major 3's stated chain — "report fallback, remove it, publish the byte-fetched ref" — does not hold. An
   allocation failure raises `MEMORY_LIMIT_EXCEEDED`, which is neither `ABORTED` nor `NETWORK_ERROR`, so it
   propagates instead of becoming a fallback; and `abandon` appends a PRECOMMIT removal, which cannot undo
   a committed ref. `isTerminal()` is also not discriminating, since abandoning an already-promoted build
   succeeds. The defect was real; its mechanism was not as described.

**RESIDUAL WINDOW, flagged not fixed:** `eraseView` still runs after the durable commit and can throw, and
`ContentAddressedTransaction::publishStaging`'s `out_slot` is assigned only after `promoteBuild` returns.
Small, pre-existing, and unnamed by the review. Recorded here so it is not lost — closing it means
extending the same no-throw-after-commit discipline one frame outward.

**LESSON for how these reviews are consumed:** a strong review is evidence, not instruction. Two of its
four remedies were wrong in ways that would have made things worse, and I forwarded one of them verbatim.
Prescriptions from a reviewer deserve the same verification as claims from an implementer.

## RESOLVED — `ca-fsck` never printed `corrupted_runs` (found 2026-07-26, closed 2026-07-30) {#fsck-corrupted-runs-invisible}

Found while wiring the soak's signal capture. `FsckReport::clean()` requires `corrupted_runs == 0`, but
`programs/disks/CommandFsck.cpp` does not print the field on the summary line — it prints `stale_edge` and
its neighbours and omits this one. So a corrupt source-edge run is invisible to every consumer of the
applet: the summary says nothing, and the only observable form is a `corrupted-run` DETAIL row, which only
a `--detail` scan produces.

The harness counted those detail rows and warned, which was a workaround for a one-line product fix.

**RESOLVED.** `corrupted_runs` is on the summary line, is one of the five findings whose nonzero value makes
`CommandFsck::executeImpl` exit nonzero, and is a row of `kFsckHardFindings` — from which `FsckReport::clean`
is now computed, with a `static_assert` that trips in three translation units when the list grows. Closed by
the Task 1c fix rounds. Kept rather than deleted because this entry is the origin of the recurrence the fence
exists to end, and later text cites it. Note the ORIGINAL text above still describes the defect in the
present tense; it is history, not state.
Same family as everything else this week: the information exists, the reporting path drops it, and the
result reads as absence rather than as an error.

Also worth noting alongside: `ca-fsck` exits 0 on `stale_edge > 0` — only `dangling` and
`snapshot_oracle_mismatches` throw — so the soak checkpoint's existing `exit_code != 0` gate never covered
that class either. The new checkpoint assert is the only gate, deliberately (see
{#q3-stale-edge-nonfatal}: the user chose non-fatal for the applet).

## GC performance, FIRST MEASUREMENT (2026-07-26) {#gc-perf-first-measurement}

No dedicated performance test has been run — this is the metric study the user scheduled for after the
soak ({#q5-gc-introspection-now}), mined from the per-phase rows the 4 h run collected over 1,673 round
attempts. It is the first quantitative answer to "where does GC slow down", a question that had no
answer at all before this week because the GC carried no timers.

### Where the time is

| phase | rows | total | p50 | max |
|---|---:|---:|---:|---:|
| `fold_ref_intake` | 126 | **2998 s** | 21 ms | **1830 s** |
| `fold_ref_list` | 128 | 1355 s | 98 ms | 491 s |
| `defer_decision` | 253 | 1346 s | 98 ms | 431 s |
| `orphan_sweep` | 123 | 975 s | **383 ms** | 526 s |
| `ref_object_cleanup` | 123 | 785 s | 0 ms | 500 s |
| `fold_reduce` | 125 | 760 s | 7 ms | 372 s |
| `manifest_deletes` | 123 | 724 s | 0 ms | 556 s |
| `lease` | 2255 | 104 s | 2 ms | 52 s |

Two shapes, and they call for different responses. `fold_ref_intake` has a p50 of 21 ms and a max of
**1830 s** — five orders of magnitude; the median round is free and the whole cost lives in the tail.
`orphan_sweep` is the opposite: the worst MEDIAN at 383 ms, a steady toll on every round. `lease` runs on
all 2,255 rounds and costs nothing — worth knowing so nobody optimises it.

### The intake tail is LINEAR in the ref-log backlog

| logs folded | duration | ms per log |
|---:|---:|---:|
| ~0 (120 rounds) | 21 ms | — |
| 5,000 | 28.6 s | 5.72 |
| 15,000 | 116.0 s | 7.73 |
| 95,000 | 260.2 s | 2.74 |
| 120,000 | 245.3 s | 2.04 |
| 130,000 | 507.0 s | 3.90 |
| 400,000 | 1829.6 s | 4.57 |

**Weighted mean 3.9 ms per ref log ⇒ ~256 logs/s sustained fold throughput.** The per-log cost is flat
across two orders of magnitude of backlog, which is what one object-store GET per log looks like — and the
fsck measurement on the same store put ~70% of wall time in I/O wait, not CPU.

### What this settles

The CI throughput collapse ({#gc-throughput-collapse-2026-07-25}) was RCA'd as a queue-stability crossing
and is now MEASURED: the fold's service rate is ~256 logs/s, so if ref logs arrive faster than that the
backlog grows, the next round is longer, and more accumulates while it runs. The 400k-log round taking
half an hour in a single phase is that loop at work. Nothing about it is mysterious any more; it is a rate
mismatch with a number attached.

### Caveats, stated because the numbers look cleaner than they are

- The data comes from chaos runs. Node freezes and kills inflate tails; some part of the max column is a
  frozen process, not work. Separating those needs a no-chaos run, which has not been done.
- Sample sizes at the top of the backlog range are n=1 per bucket. The linearity rests on six points.
- Per-log cost is presumed to be one GET each; that is inferred from the flatness and the I/O-bound
  fsck measurement, NOT from counting requests. The per-phase rows carry `ProfileEvents`, so the request
  count per phase is available and would settle it — that is the cheapest next step.

### CORRECTION, same day: it is NOT one GET per log — it is 2.5-4.7, and the ratio GROWS {#gc-perf-gets-per-log}

I wrote above that the flat per-log cost "is what one object-store GET per log looks like", and flagged
that it was inferred from flatness rather than counted. Counted it — the `ProfileEvents` were already on
every phase row — and the inference was wrong:

| logs | secs | S3 GETs | GETs/log | ms/GET |
|---:|---:|---:|---:|---:|
| 5,672 | 28.6 | 14,706 | 2.59 | 1.94 |
| 16,421 | 116.0 | 52,055 | 3.17 | 2.23 |
| 96,167 | 260.2 | 328,157 | 3.41 | 0.79 |
| 123,057 | 245.3 | 313,128 | 2.54 | 0.78 |
| 132,618 | 507.0 | 611,216 | 4.61 | 0.83 |
| 404,065 | 1829.6 | 1,912,078 | 4.73 | 0.96 |

**Weighted: 4.29 GETs per log at ~0.94 ms per GET.** What is actually flat is the PER-REQUEST cost — a
store round trip — while requests per log range 2.5 to 4.7 and trend UPWARD with backlog size.

That changes where the lever is. The cost is not "one unavoidable read per log"; it is a request
multiplier of four-ish that nobody has looked at. `foldManifestEdges` GETs a manifest BODY per manifest
edge (`CasRefManifestBodyFoldGets` exists precisely to count that), so a log carrying several edges costs
several GETs, and the same manifest body may be re-read across logs within one round with no cache in
between. Reducing the multiplier is a different and probably cheaper intervention than anything aimed at
the logs themselves.

Also confirmed by the same rows: `S3ListObjects = 0` throughout `fold_ref_intake` — all the listing is in
`fold_ref_list`, so the phase split is clean and the two costs are genuinely separable.

Still not measured: whether the manifest-body re-reads are the multiplier. `CasRefManifestBodyFoldGets` is
already recorded per phase, so this is again a query rather than an experiment.

### ATTRIBUTED 2026-07-26: the multiplier is manifest edges, and every edge costs a HEAD *and* a GET {#gc-perf-multiplier-attributed}

Task #9 answered by query, as predicted — no experiment needed. The counters were on the rows all along.

**`S3GetObject` = `CasRefLogBodyGets` + `CasRefManifestBodyFoldGets`, exactly, on all six rows.**
5672+9034=14706 · 16421+35634=52055 · 96167+231990=328157 · 123057+190071=313128 ·
132618+478598=611216 · 404065+1508013=1912078. Nothing else in the phase reads from the store.

So the composition is not mysterious. It is **one GET per ref-log body** — my original inference, correct
for the log itself — **plus one GET per emitted manifest edge**. The "4.15 GETs per log" was never a per-log
cost; it was `1 + edges_per_log`, and `edges_per_log` runs 1.54 → 3.73 and CLIMBS with backlog. That climb
is the whole growth story.

**And every edge costs a second round trip.** `CasManifestHead` = `CasManifestGet` =
`CasRefManifestBodyFoldGets` = `CasRefEmittedEdges`, exactly, on every row. Each edge is a HEAD followed by
a GET. Total round trips per phase = `logs + 2 × edges`:

| logs | edges/log | round trips | ms/trip |
|---:|---:|---:|---:|
| 5,672 | 1.59 | 23,740 | 1.20 |
| 96,167 | 2.41 | 560,147 | 0.46 |
| 132,618 | 3.61 | 1,089,814 | 0.47 |
| 404,065 | 3.73 | 3,420,091 | 0.53 |

**~0.5 ms per round trip at scale**, flat across a 140x range of request counts. The intake phase is a
straight function of its request count and nothing else — no hidden CPU term, no lock term.

**The equality `manifest body GETs == emitted edges` is itself the finding.** It means there is NO caching
of manifest bodies within a round, by construction: if the same manifest is named by ten edges, its body is
fetched ten times. Whether that actually happens is the one thing still uncounted — it needs a DISTINCT
manifest count against the edge count, which no counter currently carries. That is the next question, and
it is the difference between "the work is irreducible" and "three quarters of it is repeat reads".

**Do NOT read the HEAD-before-GET pair as a removable step.** It is a protocol step; the standing user veto
on treating protocol steps as cheap wins applies (see [[feedback_head_before_put_protocol_untouchable]]).
It is recorded here as a measured cost, not as a proposal.

### Structure behind the multiplier, and a SHARPENED hypothesis for the distinct-manifest question {#gc-perf-intake-structure}

More read-only decomposition of the same six rows. Structural facts, each an exact counter reading:

- **`txns_opened == logs_intended`, exactly, every row.** One ref-log transaction per log object. No batching
  and no fan-out at that level.
- **`deltas_emitted / edges` ≈ 14-20**, stable across a 70x range of round sizes. Each manifest fetched
  yields on the order of sixteen in-degree deltas — consistent with a part manifest naming ~16-20 files.
  The delta volume is large in absolute terms (24.6M for the 404k-log round) but it is CPU-and-memory work,
  not store traffic; the store traffic is entirely `logs + 2 × edges` as established above.
- **`tables_scanned` is 2-8.** The ref-table dimension is negligible; nothing in this phase scales with it.

**The hypothesis this sharpens, stated as a hypothesis.** `edges_per_log` runs 1.54-3.73. A transaction that
publishes one ref and drops another names two manifests and therefore costs two fetches — and if the add and
the drop concern the SAME manifest, that is two fetches of one body, a 50% waste on that transaction alone.
There is a prior for exactly this shape in the writer path: [[project_part_removal_repoint_waste]] found
repoints on `delete_tmp_*` refs accounting for ~22% of the writer PUT class.

**This is NOT established.** No counter distinguishes "two manifests" from "one manifest twice", which is
precisely why task #17 exists. What the structure adds is a specific thing to look for rather than a general
suspicion, and a reason to expect the answer to be non-trivial rather than a tidy 1:1.

It is also testable READ-ONLY before any counter is added: decode a sample of `_log/` objects on the stand
and check whether the manifests named within a single transaction repeat. That would answer the cheap half
of #17 without a product change.

## ANSWERED 2026-07-26 — task #17: 39.6% of intake manifest fetches are re-reads, and the mechanism is exact {#gc-manifest-reuse-measured}

Answered the cheap way, as {#gc-perf-intake-structure} proposed: decoded the ref-log objects on the stand
with `ca-inspect` and counted. **No product change was needed to get the answer**, so the counter proposed
in task #17 is not required for the decision (it may still be wanted for continuous tracking — separate
question).

Population: **all 959 ref-log transactions** currently in the soak pool, 8 namespaces, 6 epochs. Not a
sample.

### The rule that decides the cost {#reuse-rule}

`manifestEdgesOfTxn` (Pool/CasRefProtocol.cpp) emits at most one edge per `OwnerTransition`, by shape:

| shape | edge | manifest fetched |
|---|---|---|
| `AddPrecommit` | `+1` on the new manifest | yes |
| `RemovePrecommit` / `RemoveCommitted` | `-1` on the old manifest | yes |
| `Promote` (Precommit -> Committed, same manifest) | **none** — "the manifest keeps an owner the whole time, so there is no net edge" | **no** |

The `Promote` exemption matters and I initially got it wrong: my first pass charged two fetches to every
promote (it names the same manifest in both bindings) and produced 66.6% redundancy. Reading the emission
rule instead of assuming it removed 6,112 phantom fetches and brought the number to 39.6%. **The measured
number below is the one derived from the actual rule.**

### What was measured {#reuse-numbers}

```
transactions                      959
shapes            AddPrecommit 2991 · RemoveCommitted 4574 · Promote (no edge) 3056
EDGES emitted (== manifest GETs)  7565      = 7.89 per txn
distinct manifests among edges    4573
  redundancy                      39.6%
  repeat histogram                {1: 1582, 2: 2990, 3: 1}
intra-transaction redundancy      0.0%      <- every edge WITHIN a txn names a distinct manifest
manifests with both +1 and -1     2991      <- fetched once to add, once to remove
```

**The waste is entirely cross-transaction and the mechanism is exact.** 7565 − 4573 = 2992 redundant
fetches; 2991 manifests carry both an add and a remove edge in this window. A part's manifest body is read
once when its ref is published and again when its ref is dropped — the same bytes, for the same blob list,
because there is no cache between them (proven independently by `CasManifestGet == CasRefEmittedEdges`
exactly, in {#gc-perf-multiplier-attributed}).

### What this does and does not license {#reuse-conclusions}

A **round-scoped manifest-body cache** is a real lever: it would eliminate ~40% of the dominant phase's
store traffic on this workload, and each avoided edge saves TWO round trips (HEAD + GET), not one. An
op-scoped or transaction-scoped cache would save **nothing** — intra-transaction redundancy is exactly zero.

Three honest caveats:

1. **This is the residual pool (959 logs), not the measured rounds (5k-404k logs).** Direction of the bias
   is arguable but not measured: a larger fold window should capture MORE add/remove pairs in one round,
   pushing redundancy up, not down. That is a prediction. It should be checked against a large round before
   any cache is sized.
2. `edges/txn` here is 7.89 against 1.54-3.73 observed in the soak rounds — a different transaction mix
   (this pool holds bulk-drop transactions of up to 297 ops). The REDUNDANCY FRACTION is the transferable
   quantity, not the per-txn rate.
3. A cache needs a memory bound; manifests are not small, and the fold already holds per-round buffers.

Task #10 (the rig) was blocked on this and is unblocked: the service rate to design against is the measured
256 logs/s, with a known ~40% headroom available from caching rather than from protocol change.

## FIXED 2026-07-26 — task #13: fsck could not report on a large pool, in four separate ways {#fsck-large-pool-fixed}

Opened as "the `corrupted_runs` one-liner plus the 180 s budget". Measuring first turned it into four
defects, two of which were worse than the ones on the card.

### 1. `corrupted_runs` was invisible AND non-fatal {#fsck-corrupted-runs-fixed}

Counted since the seal check landed, a term of `FsckReport::clean`, rendered in `--detail` rows — and
absent from the summary line, which is the only thing the harness parses, CI greps, or an operator reads.
It also did not make `ca-fsck` exit nonzero, unlike the other two `clean()` terms. So a GC source-edge run
failing its whole-file seal checksum was invisible twice over, and no run has ever reported one.

Both fixed. The summary line moved out of `CommandFsck::executeImpl` into `Cas::formatFsckSummary`, which
exists so the line is reachable from a unit test at all — `CasFsckSummary.EveryHardFindingAppearsOnThe
SummaryLine` is written against `clean()`'s own terms, so the NEXT hard finding added without rendering
fails there instead of hiding for months. Verified failing before the fix, not just passing after.

### 2. The two timeout budgets were INVERTED, which made `--partial` unreachable {#fsck-partial-inversion}

The harness bounded the SUBPROCESS at 180 s while `ca-fsck`'s own scan deadline defaulted to 600 s. The
process was therefore always killed before its internal deadline could fire — and that internal deadline
is the only path that prints accumulated `partial=1` counts. `--partial` existed in the product the entire
time and could not be reached from the caller.

Measured cost in the 4-hour Part B soak: **4 of 39 checkpoints lost their whole post-GC fsck gate**
(`dangling`, `stale_edge`, dryrun-subset — all skipped), plus 4 entry-gate skips. The gate drops out
exactly when the pool is big, which is when it is worth having.

Fixed by placing the scan deadline strictly inside the subprocess budget (`PARTIAL_MARGIN_S = 20`) and
passing `--partial` from the timeout-prone call sites.

### 3. The fix would have introduced a fabricated consistency proof {#fsck-partial-gate-hazard}

Caught by writing the regression test before believing the fix. With `partial=True` a timed-out scan now
RETURNS `dangling=0, exit_code=0` instead of raising — and `wait_for_pool_consistent` counts exactly that
as a clean read. Turning on partial would have converted an honest timeout into a fabricated coherent cut:
the precise failure the partial work exists to remove. `clean` now also requires `not partial`, and
`stale_edge_verdict` returns `unchecked` on any partial result. A positive finding is still checked BEFORE
the partial gate — being partial weakens proofs of ABSENCE, never evidence of PRESENCE.

### 4. The `M-F debris, B140` label was still printed 40 times per run {#fsck-mf-debris-label-removed}

The attribution is wrong — the product classifies these as `AwaitingGc`, the ack-floor pipeline mid-flight
— and believing that label is what hid the retention leak. Three output sites, not the two a first
`grep | head -3` showed; the third was found only by re-grepping the whole file. Explanatory docstrings in
`checker.py`, `run.py` and `plot.py` still carry the B140 rationale and are NOT fixed here.

### Residual, recorded rather than half-fixed {#fsck-fabricated-clean-on-timeout}

On the remaining `FsckTimeout` path the harness still substitutes `{"dangling": 0, "unreachable": 0, ...}`
— fabricated zeros. Every consumer past that point is guarded by `not _detail_fsck_skipped`, so no assert
reads them today; it is a landmine, not a live defect, and it is now commented as one at the site. Removing
it means auditing every downstream `f.get(...)`, which is a change with real regression surface and does
not belong bolted onto this one.

### Also fixed: the harness suite had 4 pre-existing RED tests {#soak-suite-stale-reds}

Found while running the suite for this task, unrelated to it, all stale tests rather than product defects —
each the tail of a deliberate 2026-07-22 change nobody updated the test for: `lazy_load_tables` asserted as
emitted after it was turned off; a `FakeNode` answering only `ping` after the readiness gate started
proving table load with a real read; an error-message assertion pinned to the pre-2026-07-22 wording. A red
suite is worse than no suite — nobody can tell signal from noise in it. Now 275 pass, 0 fail.

## Probe A, task #12: the hypothesis space is now THREE, and the third one is new {#probe-a-direction-evidence}

### CORRECTION FIRST: my own "zero probe A lines" reading was a masked permission error {#probe-a-permission-error}

I grepped the soak's server logs from the host, got `0` everywhere, and wrote in the worklog that the
current logs hold no probe A lines. The files are `-rw-r----- syslog:syslog`; my user is not in `syslog`.
Every one of those greps was PERMISSION DENIED, and `2>/dev/null || echo 0` turned each denial into a
confident zero. **That is the project's recurring failure shape, produced by my own shell.** Re-run inside
the containers, the same logs hold 177,276 probe A lines on ch2's current log alone. Never let `|| echo 0`
stand in for a command that can fail for reasons other than "no match".

### Lost-lease: REFUTED {#probe-a-lease-refuted}

Correlated all seven surviving firings against `gc_fence` / `gc_fence_out` / `mount_remount` /
`mount_conflict`. **No lease-class event falls inside any firing window**, on either node, including the
244,939 instance; a ±60 s halo finds nothing for it either. `gc_fence` fired 400 times on ch1 and 4,245 on
ch2 during the run, so the logging is live and its silence here means something. My earlier claim that the
giant instance was "very likely a lost-lease artifact" is withdrawn.

(An intermediate query returned `NULL` rather than `0` from a correlated subquery. Reading that as "no
events" would have reached the same conclusion by accident. Redone offline against the full event list.)

### The direction split, which is the real evidence {#probe-a-direction-split}

Probe A logs every hole with its id AND its direction. The two directions are not equally informative:

| direction | ch1 | ch2 | what it excludes |
|---|---:|---:|---|
| missing from the FOLD's scan (walk 2) | 0 | 338,559 | nothing — concurrent deletion explains it |
| missing from the PRE-FOLD scan (walk 1) | **30** | **28** | deletion cannot: walk 2 SAW the object |

For the second direction the object demonstrably existed, and its id is below what walk 1 had already
observed for that namespace, so walk 1 should have returned it.

**Stale-epoch writer is also excluded.** Every one of the 58 ids carries epoch `0x4`, with near-consecutive
sequences (`0x1f171`, `0x1f173`, `0x1f174` inside one namespace at one instant). A writer at an older epoch
would be the only way to mint an id below `pre_max` after walk 1 — and these are not from an older epoch.

### The third hypothesis, which the probe's own message does not consider {#probe-a-third-hypothesis}

The log line asserts "an append cannot explain this". That reasoning holds only if appends become VISIBLE
in sequence order. If a ref-log PUT can still be in flight while a later-sequenced PUT has already landed,
then walk 1 legitimately sees `0x1f180` and legitimately misses `0x1f174` — no listing hole required, and
walk 2 later sees both.

This is not idle: `appendRefOps` uses a leader/batch model (`pending.push_back`, a leader carves and
flushes a batch) and the queue mutex is released around the flush. Whether one leader's flush can issue
several ref-log PUTs whose completions are observable out of order is the question, and it must be READ
rather than assumed.

**So #12 is not settled, but it is much better posed.** Either the object store returned an incomplete
prefix — the release blocker {#list-as-journal-dataloss-2026-07-25}, observed rather than modelled — or
the probe's justification has a hole and 58 of its firings are false positives that abort folding for
nothing. Both outcomes matter, and they are distinguished by one question about the append path.

**Next step, precisely:** determine whether a single leader's batch flush can have two ref-log PUTs in
flight simultaneously for the same namespace. If it cannot, in-order visibility holds, the third hypothesis
dies, and the 58 holes ARE the blocker observed in the wild.

### #12 ANSWERED: all four alternatives are excluded — the store returned an incomplete prefix {#probe-a-answered}

Continuing {#probe-a-direction-evidence}. The 58 "missing from the pre-fold scan" holes survive every
alternative explanation, each excluded on evidence rather than plausibility:

1. **Concurrent deletion** — excluded by direction. Walk 2 SAW the object; deletion cannot make something
   reappear.
2. **A stale-epoch writer minting an id below `pre_max`** — excluded by the ids. All 58 carry epoch `0x4`
   with near-consecutive sequences, not an older epoch.
3. **Two appends in flight at once, completing out of order** — excluded by the append path.
   `leader_active` is per-`RefTable`, guarded by `ref_queue_mutex`, so one leader per namespace; the leader
   PUTs via `putIfAbsentControlled`, which is synchronous and awaited; a carved chunk seals to exactly ONE
   ref-log object. The PUT for W therefore completes before the PUT for X is issued. This is the
   hypothesis the probe's own "an append cannot explain this" implicitly assumes away — it is true, but it
   needed checking rather than asserting.
4. **An `Unresolved` PUT landing late, after resolution declared the id a free gap** — excluded by design,
   and the design says so explicitly: "`resolveByExactGet` never reports a plain absent verdict: an absent
   or unreadable key returns `Unresolved`, since another attempt may still be legal." An absent key does
   NOT free the id; the lane stays wedged, so no later append can get ahead of a still-flying one.
   (`CasConditionalWriteUnresolved` fires 416 times on ch1 in a few hours, so this path is well travelled
   — worth excluding rather than waving away.)

**What remains is the probe's first-named explanation: the object store gave two different answers about
the same durable prefix.** That is {#list-as-journal-dataloss-2026-07-25} — until now mechanised in TLA+
and argued from first principles — **observed in a running system.**

### Two things this does NOT establish {#probe-a-answered-limits}

**It is RustFS, not AWS S3.** Everything here is against the test object store the soak and CI run on. It
does not show that S3 behaves this way. That changes how alarming the observation is; it changes nothing
about the design conclusion, because the GC must not depend on LIST completeness in the first place — which
is precisely what the blocker says. A store that can do this exists and we run on it daily.

**Exclusion 3 rests on reading the code, not on an experiment.** The reasoning is short and the invariants
are documented in the source, but a reader who disagrees should attack that link first.

### The reassuring half {#probe-a-detector-worked}

Every one of the 7 firings ABORTED ref folding: no cursor advanced, no destructive action ran. The detector
built earlier this round did exactly the job it was built for, on its first live outing, against the defect
it was aimed at. The blocker's blast radius — a cursor advancing over records a round merely OBSERVED —
did not occur because the probe stopped it.

### Confirmation step worth doing cheaply {#probe-a-store-experiment}

A standalone hammer against RustFS — write a known key set, LIST the prefix repeatedly under concurrent
writes, diff each answer against the known set — would confirm the store-side behaviour directly, without a
soak. That belongs with the rig (#10) and is much cheaper than one.

### #18 in progress — and a GAP in my own experiment design, stated before the result {#probe-a-hammer-design-gap}

The add-only hammer is running: 380k-key, 382-page listings under 6 concurrent writers, **zero holes across
the first 21 rounds** (~5M keys listed cumulatively). That is already the page-count regime probe A's
firings came from.

**But the experiment is not yet a fair model of the CAS ref prefix, and the difference is the most likely
discriminator.** My hammer only ADDS keys. The real ref prefix has objects being DELETED concurrently —
GC removes folded logs — and deletion during a paginated walk is the classic source of listing anomalies:
a continuation token can name a position whose key is gone by the time the next page is fetched, and how a
store handles that is exactly where implementations differ.

So a zero result from THIS run does not weigh against {#probe-a-answered}; it only rules out the add-only
regime. The run that matters adds a DELETER thread removing keys from behind the listing cursor while it
walks. That is the next configuration, and it should have been the first.

Recording the gap before the verdict lands, so the verdict cannot be quietly reinterpreted to fit.

### #18 RESULT: both direct regimes are CLEAN — which weakens {#probe-a-answered} {#probe-a-hammer-negative}

Two valid runs against RustFS, using probe A's own witness rule:

| regime | rounds | pages/listing | keys listed | deleted under each walk | HOLES |
|---|---:|---:|---:|---:|---:|
| add-only | 24 | up to 888 | 6.85M cumulative | — | **0** |
| held population + deletion behind the cursor | 40 | 151-166 | ~6.2M cumulative | ~31,000 | **0** |

(A third run is excluded as worthless: unthrottled deleters drained it to a single key by round 31, so most
rounds listed 1-3 keys and could not have produced a hole. Recorded so nobody counts it as a third clean
result.)

**This is a real negative and it counts against my own conclusion.** {#probe-a-answered} reached "the store
gave two different answers about the same durable prefix" by ELIMINATION. Two direct attempts in the two
obvious regimes — the second of them a deliberate model of GC deleting from behind the listing cursor —
found nothing at ~13M keys listed, more than nine times what probe A saw across its whole four-hour run.

So one of these is true, and the next step is to find out which:

1. **An eliminated hypothesis was eliminated wrongly.** The weakest link remains the in-flight-append
   exclusion, which rests on reading the append path rather than on an experiment.
2. **The hammer still differs from the CAS walk in a way that matters.** Three candidates, in my order of
   suspicion:
   - **RETRIES INSIDE A PAGINATED WALK.** CAS lists through the ClickHouse S3 client, with its own retry
     and timeout budget; my hammer uses boto3 with 3 attempts against a healthy store. A page request that
     errors and is retried mid-pagination is a completely untested path, and it is the one place a page
     could be silently skipped.
   - **CHAOS.** The soak froze and killed RustFS during runs. My hammer ran against a store nobody was
     attacking. A pagination interrupted by a store restart may not honour its continuation token the same
     way.
   - **Key shape.** CAS keys are nested paths containing `@cas@`; mine are flat `k-NNNNNNNN`. Least likely,
     but not excluded.

**Cheapest next test, and it uses evidence already on disk:** correlate the probe A firing windows against
RustFS faults and S3 request errors during the soak. If every firing sits inside a fault window, candidate
2 is the answer and the store is innocent under normal operation — which changes the blocker's urgency
without changing its validity, since a GC that trusts LIST completeness is still wrong.

### The holes are NOT page-shaped — which rules out the model everyone would reach for {#probe-a-hole-shape}

Decoded the ids and namespaces out of one firing (ch1, 06:06:09, 13 holes):

```
11 holes  span 0x1f171..0x1f17f  (15 id slots)  ns ca_soak_ch1/store/162/...
 2 holes  span 0x1119c..0x1119d  ( 2 id slots)  ns ca_soak_ch2/store/243/...
```

Two TIGHT CONTIGUOUS CLUSTERS of adjacent keys, 13 in total, in two namespaces at the same instant. (The
gaps inside the first span — `1f172`, `1f177`, … — are not evidence of interleaving: `appendRefOps`
legitimately leaves id gaps, "the id is a safe gap", on its conclusive-rejection path.)

**A dropped LIST page would be ~1000 keys.** This is thirteen. So "the store lost a page" — the natural
reading of {#probe-a-answered}, and the one the probe's own message suggests — does not fit the data. Ref
log keys are `<epoch hex>-<seq hex>.zst` zero-padded, so lexicographic order IS id order and these are
physically ADJACENT keys in the listing. Walk 1 returned the keys after `0x1f17f` and skipped the short run
before it.

Two consequences:

1. **It also rules out classic offset-pagination skew**, which the deletion regime would have exposed:
   `del2` deleted ~31,000 keys from behind the cursor across each of 40 walks and produced ZERO holes. A
   store that shifted its cursor on deletion would have failed that test loudly.
2. **The mechanism drops SHORT ADJACENT RUNS, not pages.** That is a much narrower target than "the store
   is inconsistent", and it is the shape any explanation now has to produce.

I do not have a mechanism that predicts this shape yet. Stating that plainly rather than picking whichever
of the surviving hypotheses is least disproved — the honest position after {#probe-a-hammer-negative} is
that the cause is UNKNOWN, with the LIST-completeness reading weakened by two direct experiments and the
elimination argument weakened by this shape.

**Where it goes next:** the CAS walk lists through the ClickHouse S3 client under chaos, my hammer used
boto3 against a healthy store. A retried page request is the one path that could plausibly return a
slightly different window of keys. That is now the leading candidate and it is testable — inject page-level
errors into the hammer's client and see whether short adjacent runs start disappearing.

### THREE valid hammer runs, all negative — stop guessing and make the detector self-diagnosing {#probe-a-hammer-three-negatives}

| run | pagination | rounds | pages | keys listed | deletes under each walk | HOLES |
|---|---|---:|---:|---:|---:|---:|
| add-only | continuation | 24 | ≤888 | 6.85M | — | 0 |
| held population | continuation | 40 | 151-166 | 6.2M | ~31,000 | 0 |
| held population | **start-after (what CAS does)** | 40 | 151-166 | 6.08M | ~15,500 | 0 |

~19M keys listed. Nothing. Two more code-level hypotheses died on inspection in the same stretch:

- **Page-limit mismatch between the two walks** — no: both use 1000 (walk 2 passes it explicitly, walk 1
  takes the default). The stitch points still DRIFT between walks, because the key space mutates and the
  count of keys before any given key differs — so a store that disagreed with itself at a stitch would
  produce asymmetric holes. That remains a mechanism; it is just not a page-size bug.
- **The two walks filtering keys differently** — no. Walk 1 selects with `parseRefObjectKey` inline; walk 2
  collects raw keys and filters later via `groupRefKeys`. Both call the same parser. The only asymmetry is
  STRICTNESS: walk 1 silently skips an unparseable key, walk 2 throws `CORRUPTED_DATA` and aborts the
  round. That is worth tidying, but it produces an abort, never a hole.

**The approach is wrong, not just the hypotheses.** Every attempt so far reconstructs the crime scene after
the fact from ids in a log. The detector is already at the exact moment the disagreement exists and throws
that moment away.

**Change probe A to answer the question at firing time.** When a hole is found, immediately `HEAD` the hole
key and record the verdict in the same log line:

- **object EXISTS** → walk 1 missed a durable object. The listing was incomplete; store or client.
- **object ABSENT** → walk 2 returned a key that is not there. A phantom, which points at the client or
  iterator, not at LIST completeness — and would invalidate the whole reading of {#probe-a-answered}.

One HEAD per hole, and holes are rare by construction (7 firings in four hours). It converts the next
firing from a forensic puzzle into a decisive observation, and it is far cheaper than another 19M-key
hammer run.

## WHY the audit log could not trace this, which is itself the defect (user challenge, 2026-07-26) {#anomaly-detail-is-a-bare-count}

Asked why `system.content_addressed_log` could not settle the probe A mechanism, given that this project's
convention is to write the FULL CONTEXT of every critical decision into `detail`. Checked. The convention
is not being followed at the one event that matters:

```
gc_fold_end   detail = {anomalies: '1', shards: '8'}
gc_fold_begin detail = {}                                  -- empty, every row
```

`Gc::Report::recordAnomaly` takes `(namespace, shard, ManifestId, reason)`. **None of it reaches the audit
log.** The row carries a COUNT. Probe A ABORTS ref folding — no cursor advance, no destructive action — and
the queryable record of that decision is the number 1.

Three consequences, all of which I hit this round without naming the cause:

1. **The count cannot even identify WHICH anomaly fired.** A probe A disagreement and an undecodable
   ref-log body both land as `anomalies: 1`. There is a `gc_fold_end` at 08:30:35 on ch1 that does not
   appear among the probe A firings in the per-phase rows, and I cannot say what it was.
2. **The real context exists only in the TEXT log**, which is rotated, compressed, syslog-owned and
   unreadable from the host — which is how I produced a masked-permission "zero" earlier today
   ({#probe-a-permission-error}).
3. **So every investigation becomes forensics**: decode ids out of grep output instead of querying the
   table built for exactly this.

**Fix, and it subsumes the HEAD-at-firing-time step in {#probe-a-hammer-three-negatives}:** give the
anomaly its own audit event carrying its context in `detail` — `reason`, `namespace`, the hole id, the
DIRECTION (which enumeration missed it), the other enumeration's max id for that namespace, and the
HEAD verdict on the hole key (exists / absent). Then the next firing is one `SELECT` away from a mechanism
instead of an afternoon of log archaeology.

That is the correct next step for {#probe-a-answered}, and it is a smaller change than the three hammer
runs it would have replaced.

### #20 DONE: the anomaly now records what it saw {#anomaly-detail-fixed}

`gc_anomaly` event (namespace, hole id, direction, both enumerations' maxima, hole ordinal), two
ProfileEvents (`CasGcProbeAHolePresent` / `CasGcProbeAHoleAbsent`), and the verdict in the log line.

Three carriers on purpose, each covering another's blind spot: `EventEmitter` no-ops when no audit sink is
installed, the text log is rotated and root-owned (which already produced one false "zero occurrences"
today), and only the counters are readable by the soak harness, CI and `system.events`. Both counters are
registered in `soak/signals.py`, so preflight fails rather than silently reading zero.

Capped at 32 rows/round with the cap and true total in every row, plus an explicit line naming what was
dropped. Test pins the DIRECTION of the verdict and was verified failing both ways — not taken, and
inverted. Widened gate green (1379).

**What this buys: the next probe A firing is one `SELECT` from a mechanism.** `present` means an
enumeration omitted a durable object — {#list-as-journal-dataloss-2026-07-25} observed. `absent` means one
returned a key that does not exist, which would invalidate that reading entirely and point at the client.
Neither is recoverable after the fact, which is why three hammer runs and ~19M listed keys could not settle
it.

## *** CAUGHT LIVE 2026-07-26: a ref-prefix enumeration omitted two adjacent objects that DEMONSTRABLY EXISTED *** {#probe-a-caught-live}

The instrumentation from {#anomaly-detail-fixed} paid off on its FIRST soak, inside the first four minutes.
Three hammer runs and ~19M listed keys had failed to reproduce this; one `SELECT` now shows it.

```
namespace    ca_soak_ch1/store/3ba/3ba2c30d-...@cas@
hole         epoch 1, seq 0x1430c   head_verdict = present
hole         epoch 1, seq 0x1430d   head_verdict = present
direction    missing from the pre-fold scan
pre_max      epoch 1, seq 0x1430e          <-- walk 1 DID return this
fold_max     epoch 1, seq 0x14a10
```

Counters agree: `CasGcRefScanDisagreements=2`, `CasGcProbeAHolePresent=2`, `CasGcProbeAHoleAbsent=0`.

**Walk 1 returned `0x1430e` and skipped the two keys immediately below it.** Both objects were confirmed
present by a HEAD taken at the moment of the disagreement. Same short-adjacent-run shape as the historical
firings ({#probe-a-hole-shape}), now with a verdict attached instead of reconstructed.

### What this settles, and what it does not {#probe-a-caught-live-limits}

**Settled: the holes are not phantoms and not deletions.** `absent` would have meant walk 2 invented a key,
which would have invalidated the whole reading; it did not happen. Deletion cannot explain a key that is
present when checked.

**NOT settled: incomplete listing vs non-serialized appends.** Two branches remain, and the HEAD verdict
cannot separate them:

1. The enumeration was incomplete — the LIST-as-journal blocker, observed.
2. `0x1430c`/`0x1430d` were written AFTER `0x1430e`, so they genuinely did not exist when walk 1 ran. That
   requires appends to a single namespace to complete out of order, which contradicts the reading of
   `appendRefOps` in {#probe-a-answered} — one leader per `RefTable`, synchronous awaited PUT. That reading
   is the weakest link in the argument and this is exactly where it would break.

**The discriminator is one more field.** `HeadResult::attributes` carries object metadata; recording the
hole object's LAST-MODIFIED time against the walk's start time decides it outright — modified before walk 1
means the listing was incomplete, full stop; modified after means the append ordering assumption is wrong
and the defect is in our writer, not the store. That is a small addition to the same event.

Do NOT close {#list-as-journal-dataloss-2026-07-25} on this. It is much stronger evidence than anything
before it, and it is still one field short of proof.

### The lesson worth more than the finding {#probe-a-caught-live-lesson}

Three hammer runs, ~19M keys listed, several code reads, an afternoon — all negative. One instrumentation
change, one 20-minute soak, four minutes in — decisive. The detector was standing at the moment of the
disagreement the whole time and throwing it away, and I went looking for the crime scene elsewhere instead
of asking it what it saw. The user's question ("why can't you trace this through the CA log, we write the
full context of every critical decision") is what redirected it.

### The second branch closes too: append ordering verified at THREE levels {#probe-a-append-order-verified}

{#probe-a-caught-live} left two branches. The second — that `0x1430c`/`0x1430d` were written AFTER
`0x1430e`, so walk 1 legitimately missed them — required appends to one namespace to complete out of order.
That rested on a single reading of `appendRefOps`, which I had repeatedly flagged as the argument's weakest
link. Re-checked properly, it holds at three independent levels:

1. **The baton.** `leader_active` lives in `RefTable`, guarded by `ref_queue_mutex`: one leader per
   namespace, and a new tenure cannot begin until the previous released.
2. **The flush loop is sequential.** `for (size_t item_index = 0; item_index < batch.size(); ++item_index)`,
   committing chunk by chunk through `commitRefChunk`. Its own failure-isolation comment states the
   ordering outright: "Earlier chunks that already committed keep their callers' success" — chunk N is
   fully committed or failed before chunk N+1 is attempted.
3. **The PUT is synchronous and awaited**, one object per carved chunk.

Ids are minted in increasing order; within a tenure chunk order is id order, across tenures the baton
orders them. **So the PUT for id N completes before the PUT for N+1 is issued.**

Apply that to the captured firing: `0x1430c`, `0x1430d`, `0x1430e` are consecutive and ALL THREE were
confirmed present. Therefore `c` and `d` were durable before `e`'s PUT even started. Walk 1 returned `e`,
so walk 1 ran after `e` landed — hence after `c` and `d` landed — and did not return them.

**The enumeration was incomplete.** {#list-as-journal-dataloss-2026-07-25} is no longer a model, an
inference, or a survivor of elimination: it is observed, with the omitted objects proven to exist and the
ordering that makes their absence impossible verified in three places.

### The one thing still resting on reading rather than measurement {#probe-a-remaining-hardening}

The ordering guarantee is established from SOURCE, not from an experiment. A reader who rejects it can
still reject the conclusion. The clean way to close that is the object's LAST-MODIFIED time against the
walk's start — and it is NOT currently reachable: `HeadResult::attributes` is `map<String,String>` of USER
metadata, not S3's `LastModified`, and the writer's `ref_publish`/`ref_drop` audit events do not carry the
ref txn id either (checked both). Surfacing either one is a real change, worth making, and it is hardening
of a conclusion rather than a gate on it.

### STILL OPEN after #13: the fsck budget, which #13 made honest rather than sufficient {#fsck-budget-still-open}

The 2026-07-26 verdict soak skipped its GC-checkpoint entry gate again: `entry-gate fsck timed out
(ca-fsck (detail=False) exceeded 180s)` on a pool of **5.5 GB**. No false "PERSISTENT dangling" this time —
that regression is fixed and the timeout now degrades to a logged skip, which is correct behaviour.

But correct behaviour here means **the gate does not run.** #13 removed the lie; it did not buy the check
any time. A 5.5 GB pool is small, and 180 s is not close to enough — the earlier measurement of
`reachable=0` after 160 s says the scan had not finished even its first phase.

This is the same shape as everything else this round: an instrument that reports honestly that it saw
nothing is better than one that lies, and still not a check. Options, none yet chosen:

- scale the budget with pool size instead of a flat wall-clock number;
- make the entry gate use `--partial` deliberately and treat the result as a lower bound (safe for the
  gate's purpose only if a partial `dangling > 0` still fails, which it would);
- make fsck itself cheaper — the intake measurements ({#gc-perf-multiplier-attributed}) suggest the same
  per-request cost dominates here.

Tracked so the skip does not become the accepted normal.

### Verdict soak: GREEN, and three things it taught beyond the catch {#verdict-soak-outcome}

`PHASE3 OK`, `SOAK_EXIT=0`. 88 signal reads, both new counters in preflight with per-node baselines, GC
phases captured at 5/6 checkpoints over 179 round attempts.

**1. The ProfileEvents counters were WIPED and the audit rows survived.** Final `system.events`:
`disagreements=0 present=0 absent=0` on both nodes — chaos restarted the servers and process-local counters
reset. `system.content_addressed_log` still holds the 2 `gc_anomaly` rows on ch1, with the ids, the
direction and the `present` verdicts intact.

**The finding this whole round turned on would have been ERASED by a restart if I had shipped only the
counters.** The audit event is what made it durable, and that was the user's point when they asked why the
CA log was not being used. Recording it because the reflex "add a ProfileEvent" is cheap and would have
quietly lost the evidence.

**2. Probe A is `reported-not-gated`** — `CasGcProbeAHolePresent peak=2, nonzero_in=28/88 reads`. A soak can
go green with confirmed enumeration holes. Defensible today: the product already fails closed (folding
aborted, no cursor advance, nothing deleted), so the run genuinely was safe. But now that the holes are
CONFIRMED rather than suspected, whether a green run should be allowed to contain them is a decision
someone should make deliberately rather than inherit.

**3. `pending_deletes` hit 77.2 SECONDS in a single occurrence** — against `orphan_sweep=242.8ms`,
`fold_reduce=223.1ms`, `fold_ref_list=60.7ms`. Three orders of magnitude above every other phase, on a
20-minute run. The GC-performance work has been looking at `fold_ref_intake` because that is what dominated
the 4-hour data; this says `pending_deletes` deserves its own look. Folded into the study
({#gc-bottleneck-study-2026-07-25}), not chased now.

### #11 and #19 are the SAME DEFECT seen from two ends {#leak-is-a-consequence-of-the-hole}

Reading `CasBlobInDegree.cpp`'s merge against the recorded root cause makes the connection plain, and it
reframes the leak fix.

In-degree is a SET of source edges. The leak is a residual `+1` whose `-1` never folded
({#unmatched-minus-one-retention-leak}: 56 blobs, exactly one residual edge each, contributing manifest
gone). There are exactly two ways a `-1` fails to cancel its `+1`:

1. **The `-1`'s ref log was OMITTED from an enumeration and the cursor advanced past it.** The log is never
   folded again — sealing a cursor above a record is permanent — so the `+1` stands forever. This is
   {#list-as-journal-dataloss-2026-07-25}, and as of {#probe-a-caught-live} it is CONFIRMED to happen.
2. **The `-1` arrived BEFORE its `+1`.** `present` is false when the remove is applied, so the remove is
   dropped as a per-key no-op; the `+1` then lands with nothing left to cancel it. This is the direction
   `CasGcUnmatchedRemoveDeltas` counts — 22 occurrences in the 20-minute verdict soak.

So the retention leak is not an independent bug to be fixed in the reducer. **It is a CONSEQUENCE**, and
the reducer is behaving correctly in both cases: a set cannot cancel an element it never received, and
dropping an unmatched remove is exactly right (the alternative — materialising a negative edge — is how a
false deletion would be born).

### What this changes about the fix {#leak-fix-reframed}

**Do not "fix" the merge.** The obvious remedy was already known to be wrong (`RefTableState` throws
`CORRUPTED_DATA` on an absent binding, turning a leak into a permanent wedge); this says something stronger
— there is nothing to fix at that layer at all.

Three separable pieces of real work instead:

- **Source, path 1:** enumeration completeness. Probe A already ABORTS folding when it catches a hole, so
  the cursor does not advance and path 1 is closed for holes it catches. Its two stated blind spots remain
  open: a hole that reproduces IDENTICALLY in both enumerations, and a namespace dropped WHOLESALE from one
  enumeration (no `ref_tables` entry, so the comparison loop never visits it).
- **Source, path 2:** whatever lets a `-1` reach the reducer ahead of its `+1`. Unexamined. 22 occurrences
  in 20 minutes is not rare, and it needs its own investigation — `CasGcUnmatchedRemoveDeltas` hands back
  one example per round (`unmatched_remove_example`), which is where to start.
- **The 56 already-leaked blobs:** a one-off reconciliation, since no incremental round can ever reclaim
  them. Only a rebuild of the in-degree state can, which is what the fsck note already says.

Task #11's framing ("fix the unmatched-minus-one retention leak") is therefore wrong as written and has
been left in place only so the history reads honestly; the work is the three items above.

## *** PROVEN BY MEASUREMENT: the enumeration was incomplete *** {#probe-a-proven-by-measurement}

I wrote in {#probe-a-remaining-hardening} that the object's write time was "NOT currently reachable" and
that the ordering guarantee therefore rested on reading source. **That was wrong.** `system.blob_storage_log`
records every object write with a microsecond timestamp, and the user asked the obvious question I had not:
what do the storage logs say?

### The three keys of the captured firing {#proven-three-keys}

```
Upload  16:47:19.211480   .../_log/0000000000000001-000000000001430c.zst   200 B   <- hole
Upload  16:47:19.212340   .../_log/0000000000000001-000000000001430d.zst   200 B   <- hole
Upload  16:47:19.213680   .../_log/0000000000000001-000000000001430e.zst   195 B   <- the witness
gc_anomaly fired at 16:47:38                                                       <- 19 SECONDS later
```

All three were written in strict id order, 2.2 ms apart. Walk 1 returned `0x1430e`, so walk 1 ran after
`16:47:19.213680` — by which time `0x1430c` and `0x1430d` had been durable for 1.3-2.2 ms, and by the time
the disagreement was reported, for nineteen seconds. Walk 1 did not return them.

### The ordering guarantee is now measured too {#proven-ordering}

Not "argued from `appendRefOps`". Across **65,263 ref-log uploads** in this namespace, partitioned by writer
epoch: **zero out of order** (65,157 in epoch 1, 106 in epoch 2). Upload timestamps rise monotonically with
id, exactly as the single-leader/awaited-PUT reading claimed.

(A first pass reported one inversion. That was my query mixing two epochs — a remount restarts the
sequence, so sorting by sequence alone puts a small post-remount id "before" a large pre-remount one.
Corrected by partitioning; the artifact was mine, not the system's.)

### So the conclusion no longer rests on any code reading {#proven-conclusion}

**A ref-prefix enumeration failed to return two objects that had been durable for nineteen seconds, while
returning a third written 2.2 ms after them.** {#list-as-journal-dataloss-2026-07-25} is proven.

### The fail-closed path also worked end to end {#proven-failclosed}

The same three keys were `Delete`d at `16:53:59` — six minutes later. Probe A aborted folding, the cursor
did not advance, a later round enumerated completely and folded them, and GC then reclaimed them normally.
**No leak resulted from this occurrence**, which is the behaviour the detector exists to produce.

## GC round duration: the answer is SERIAL REQUEST LATENCY, measured (2026-07-26 night) {#gc-round-duration-answered}

The user's three hypotheses for why a round takes tens of minutes — repeated work, unnecessary work,
serial-where-parallel-is-possible — tested against the recorded data. **The third dominates and the other
two are secondary**, with one of them refuted outright.

### The decisive arithmetic {#duration-arithmetic}

Requests per intake phase = `log GETs + edge GETs + edge HEADs`. Time divided by TOTAL requests (my earlier
0.9 ms figure divided by GETs only and ignored the HEADs, which inflated it):

| logs | secs | GETs | HEADs | requests | ms/request |
|---:|---:|---:|---:|---:|---:|
| 5,672 | 28.6 | 14,706 | 9,034 | 23,740 | 1.205 |
| 16,421 | 116.0 | 52,055 | 35,634 | 87,689 | 1.323 |
| 96,167 | 260.2 | 328,157 | 231,990 | 560,147 | **0.465** |
| 123,057 | 245.3 | 313,128 | 190,071 | 503,199 | **0.487** |
| 132,618 | 507.0 | 611,216 | 478,598 | 1,089,814 | **0.465** |
| 404,065 | 1,829.6 | 1,912,078 | 1,508,013 | 3,420,091 | **0.535** |

**The four large rounds agree within 1.15x at ~0.5 ms per request.** For the rounds that actually take
minutes, phase time is 100% accounted for by serial round-trip latency: **there is no CPU term and no lock
term to find.** The two small rounds sit at 1.2-1.3 ms and are not trusted — all this data comes from chaos
runs, and at that size a single freeze dominates.

The 30-minute round was **3.42 MILLION serial round trips**. Nothing is slow; there are simply that many,
one after another.

### Confirmed serial from source, and the code says so itself {#duration-serial}

A plain `for (auto & [ns_str, listing] : ref_tables)` with a synchronous GET per log and per edge. No
prefetch, no batching, no thread pool. GC's own comment: "one GET per new ref log plus one HEAD+GET per
manifest edge, which on a busy pool is where the round's object-read budget goes." (`meta_pool` exists but
is for async/advisory delete-side ops, not intake reads.)

### Hypothesis 1, repeated work: REAL but secondary {#duration-repeated}

- **Manifest bodies: 39.6% of edge fetches are re-reads** ({#gc-manifest-reuse-measured}). On the 404k
  round that is ~597k redundant edges x 2 trips x 0.5 ms = **~600 s of the 1830 s**. Substantial, and a
  round-scoped cache removes it without touching the protocol.
- **Fold seal: REFUTED as a cost.** The product already counts it: 26 seal reads with 13 redundant across
  all rounds on the stand — 50% redundant by COUNT, but 2 reads per round at 1-3.5 ms. It is noise. The
  standing backlog suspicion that "the fold seal is read five times per round" does not survive contact
  with the counter.

### Hypothesis 2, unnecessary work: only the HEAD, and it is off-limits {#duration-unnecessary}

Every edge costs a HEAD before its GET — **1.5 M of the 3.42 M trips on the big round, i.e. 44% of them,
~800 s**. That is the single largest identifiable block. It is a protocol step under a standing user veto,
so it is recorded as a measured cost and NOT proposed for removal. Everything else is irreducible: the log
body must be read to know the transaction, and the manifest body must be read to know the blob list.

### Hypothesis 3, serialism: THE ANSWER {#duration-serial-answer}

At ~0.5 ms per trip and 3.42 M trips, parallel fetching alone gives:

| concurrency | intake floor on the 404k round |
|---|---|
| 1 (today) | 1830 s |
| 4 | ~460 s |
| 8 | ~230 s |
| 16 | ~115 s |

Fetching is independent even though APPLICATION order is not: tables fold independently by design (a clamp
on one does not stop others), and within a table the log bodies for ids above the cursor can be prefetched
while the previous log is being applied. The ordering constraint is on the fold, not on the read.

**Combining the two levers is multiplicative**: dropping the 39.6% redundant edge fetches first shrinks the
request count, then parallelism divides what remains.

### S42 at `--scale full` does NOT FIT on this machine — a finding, not just an incident {#s42-full-scale-too-big}

Launched S42 at `--scale full` (8 writers, 4 readers, 1800 s leg-A workload). Watchdog caught it 12 minutes
in on a disk trajectory, not a hang:

```
disk free  323G -> 223G in ~30 min, then 225G -> 218G in 20 SECONDS
rate       ~21 GB/minute
remaining  18 min of leg A still to run  =>  ~380 GB needed
available  218 GB, and the alert floor is 60 GB
load average 70.5, free memory 2 GB
```

Stopped deliberately before it broke the machine. Disk stabilised at 218 GB the moment it died, confirming
S42 was the consumer.

**Three things this is worth recording for:**

1. **`--scale full` is not runnable on this host.** Any future attempt needs either a disk budget of ~400 GB
   free or a smaller scale. The scenario has no pool-size cap parameter of its own (`--max-pool-gb` exists
   on the soak driver, not on the scenario runner), so nothing stops it.
2. **A memory-fault test run with 2 GB free RAM is compromised anyway.** S42 injects allocation failures via
   `memory_tracker_fault_probability`; if the host is itself near exhaustion, real OOM kills become
   indistinguishable from injected faults and the attribution the card is built for is destroyed. The run
   needed headroom it did not have.
3. **Load average 70** on a 2-replica scenario suggests the full-scale writer/reader counts are tuned for a
   bigger machine than this one.

**Next attempt must be at `--scale ci`,** with disk headroom checked BEFORE launch and memory headroom
checked as a precondition of the fault injection being meaningful.

### `pending_deletes` at 77 s has the SAME shape — and the obvious remedy is unavailable {#pending-deletes-shape}

Phase 11/18 is a nested serial loop — per gc-shard, per retired entry — issuing one
`backend.deleteExact(blobKey, token)` per object. One round trip each, awaited, no pool, no batching. At
the measured ~0.5 ms per request, **77.2 s is roughly 150,000 deletes performed one after another.**

So intake and pending_deletes are the same story: request-bound and serial.

**The obvious remedy — S3 `DeleteObjects`, up to 1000 keys per call — is NOT available, and it is worth
recording why so nobody re-proposes it.** `deleteExact` is SAFETY-critical and token-conditional: it must
remove ONLY the incarnation whose token matches, and a wrong token must be a `TokenMismatch` with the
object untouched (`CasBackend.h`: "conditional PUTs are protocol hygiene; casPut and deleteExact are
SAFETY-critical", and backends that silently ignore the condition are rejected by `Cas::Probe`). Bulk
delete carries no per-object precondition, so batching would trade exactness — the guarantee that GC never
removes a replaced incarnation — for throughput. That is the wrong trade at any speedup.

The backend exposes no bulk path at all, which is consistent with that.

**Parallelism, however, IS available here**: N concurrent conditional deletes preserve exactness perfectly,
because each carries its own token. The same lever as intake, and the same reason it works.

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

### What the product did under a store that stopped answering {#s42-behaviour}

Worth recording separately, because it is the interesting half: faced with mass request timeouts, the write
path returned `UNCERTAIN (retry budget exhausted)` to the client and **wedged the affected ref lanes rather
than guessing**. Both lanes later unwedged. No data was lost and nothing was silently dropped. That is the
fail-closed design behaving correctly under a fault class the card was not even aiming at.

## Environment: two non-CAS reds that block an UNFILTERED `unit_tests_dbms` run (2026-07-29) {#unfiltered-unit-test-env-reds}

USER RULINGS (2026-07-29 evening): (1) `contrib/silk` fiber assert — OUT OF SCOPE, ignore
(no upstream action from this effort; keep the ASan exclusion filter). (2) root-owned
`./logs` — DELETED by the user; unfiltered runs from the repo root are unblocked.
CLOSED as attention items. Related audit note: `CANNOT_PARSE_INPUT_ASSERTION_FAILED` (28,170
in `system.errors`, zero log lines) is EXPLAINED by the user — noise from `toDateTime(...)`
inside `INSERT ... VALUES` (the fast VALUES parser fails on expressions and falls back to SQL
evaluation, counting the error each time); not a defect, removed from the audit addendum's
open questions.

Neither is ours and neither affects the CA gate, which is filtered. Both were hit while running the
convergence's ASan gate and are recorded so the next person does not re-diagnose them.

**1. `contrib/silk` fiber-scheduler assertion.** `SilkFiberSocketTest/1` (the `SecurePolicy`
instantiation) aborts the whole binary with
`contrib/silk/src/fibers/fiber.cpp:1010 assertion failed: !scheduler`. Proven CAS-independent: it
reproduces with

```
<build>/src/unit_tests_dbms --gtest_filter='SilkFiberSocketTest*'
```

i.e. with no CAS code executing at all, and `contrib/silk` was last touched by a submodule bump long
before this branch. Handling: exclude it from unfiltered runs
(`--gtest_filter='-SilkFiberSocketTest/1.*'`), which is how the convergence's whole-binary ASan pass
was obtained. Upstream-contrib issue; not on the CAS backlog to FIX, only to route around.

**2. Root-owned `./logs` in the repository root.** `CoordinationTest/0.TestSummingRaft1` refuses to
start (`Path ./logs already exists, remove it to run test`) and then terminates the binary when its
own `remove_all` throws `Directory not empty`. The directory is root-owned, dated 2026-07-03, from a
docker run, and untracked — **it needs the user's `sudo` to remove; do not attempt it from an agent
session.** Until then, run unfiltered unit tests from a scratch working directory rather than the
repository root. Filtered CA gates are unaffected (they never reach that test).

**Log-reading trap in the same runs.** An unfiltered `unit_tests_dbms` log contains embedded NUL
bytes AND can lose the head of its own redirected stdout (gtest's opening banner and thousands of
early `[ OK ]` lines simply absent from a completed log). Two consequences: always `grep -a`, and
never gate a waiter on `until grep -q "^MARKER=" "$log"` — plain `grep -q` returns 1 on such a file
where `grep -aq` returns 0, so the waiter hangs forever. Write completion markers to a separate small
text file, and count "did suite X run" DURING the run rather than from the finished file. The
trailing gtest SUMMARY block always survives, so pass/fail totals stay trustworthy.

## Numbered CAS doc set documents pre-Stage-A ref and namespace-file shapes {#numbered-docs-stale-ref-shapes}

Found by the Task 1c review, explicitly as a NON-finding of that task — it was already stale before
Task 1 and Task 1c neither worsened it nor was asked to fix it. Recorded so it is not re-discovered
as a finding a third time.

`docs/superpowers/cas/01-architecture.md`, `codecs.md`, `11-walkthrough.md`, `03-writer-protocol.md`
and `09-read-protocol.md` still document namespace files at `roots/<ns>/_files/<name>` — the
namespace-only shape that Task 1c deleted in favour of `roots/<ns>/<incarnation>/_files/<name>` — and
still show the pre-Stage-A ref shape `cas/refs/<ns>/<shard>`. `Layout::rootsPrefix`'s own doc comment
still says "root-shard manifest", vocabulary that predates the ref rework.

Do not fix these piecemeal per task: the key shapes are still moving through Stage B, so a sweep now
buys one round of accuracy and then rots again. The sweep belongs at the end of Stage B, where the
final shapes are settled — and it is the same discipline as the plan's `{#restatement-impact}`
do-not-cite list, for the same reason: a stale `refCkptKey` reference survived four tasks before
anyone swept it.

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

Behaviour half of the Task 1c review's MINOR-1 (the wording half is
`deferred-docs-fixes.md` `{#d1-parse-namespace-file-key-contract}`). Filed separately because a
finding reported as a wording problem turned out to name a real consequence, and batching it with
comment fixes would have buried it.

`Layout::mountpointObjectKey` does not enforce the `_files` reservation — its doc asserts that those
segments "never appear in a real ClickHouse loose-file path" and checks nothing. So a loose object at
`roots/<srid>/_files/x` satisfies `parseNamespaceFileKey`'s necessary condition and is treated as one
of ours. Consequence: `ca-decommission` refuses fail-close and `ca-fsck` posts a hard `lifeless_keys`
finding against a key that is not damage.

**The direction is safe** (refuse and report, never delete), which is why this is not urgent. What
makes it worth fixing is that a hard finding against a non-damaged key trains an operator to
disbelieve hard findings. Decide between enforcing the reservation in `mountpointObjectKey` and
narrowing the classifier; the first is the one that makes the existing doc true.

## RESOLVED — the stateless drain tests' `PENDING` gauge double-counted {#stateless-pending-double-count}

Found by the review of `76ee70da4a7`, ruled out of scope there because the formula is carried over
unchanged from before that commit — filed so it is not re-discovered as a new defect.

`04290_content_addressed_no_leftovers.sh` and `04295_content_addressed_mutation_no_leftovers.sh`
compute `PENDING = pending_candidates + pending_condemned + pending_retired`. But `pending_condemned`
is documented in `Gc/CasGc.h` as "their total (candidates + retired), the overall pipeline gauge" — so
the sum is exactly **twice** the true outstanding count.

**Harmless as a signal, misleading as a number.** Both tests use it only as `> 0` / `= 0`, and doubling
preserves both, so no assertion is wrong. What is wrong is every figure it prints: the failure message
`GC did not drain the retire pipeline within the bounded loop (pending=64)` reports 64 where the
pipeline held 32. Those two numbers (64 and 152) were quoted in this session's `05008` diagnosis; the
conclusion did not depend on their magnitude, but a future diagnosis might.

**RESOLVED 2026-07-30**: both tests now read `pending_condemned` alone (`8e9b06c2a81`). Done ahead of Task 7b
because it was free — neither file was touched by any in-flight work, and both tests are registered known-red,
so the change could not mask or alter a verdict. Task 7b still owes their assertion restoration.

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

Found 2026-07-30 by Task 4-C's implementer, and found the right way: by asking *why did my gate not catch
this* rather than only fixing the test that failed.

`tmp/cas_suites.txt`'s generation matches `Cas*:CA*`, which **cannot** match `CaWiring` or `CaLifecycle`
(`Ca` + lowercase matches neither `Cas` nor `CA`) and cannot match `RefWriterAppendLane`,
`RefWriterNamespaceBirth`, `RefWriterChunkedFlush` or `RefTableCacheEviction` at all. **Twenty suites, 17
of them CAS-relevant, were run by no gate pass in an entire session** — and one of the newly-included ones
immediately produced a hard abort plus further failures.

**Every gate figure reported during that session was therefore partial**: 1601, 1628, 1639, 1658, 1660 and
1661 were all computed over an incomplete set, including the deltas checked as "exact match to the new test
count". The arithmetic was self-consistent and still measured the wrong population.

**This is the third recurrence, and the reason it recurs is the shape of the previous two fixes.** Both
earlier rounds closed it by ADDING the missing suites to the list — a data fix. The list is regenerated per
session, so each fix regenerated away. The second of those recurrences hid three real bugs.

**So the fix is not a longer list.** Fix the generator or the pattern, and then add the check that makes
omission loud: **diff the generated list against the binary's own `--gtest_list_tests` output and fail when
any suite is unclaimed** — a suite must either be in the CAS set or be excluded by name with a stated
reason. A list that silently omits is indistinguishable from a list that covers, which is precisely why the
same gap has now cost three rounds and why "we added them" is not a closure.

**The generalisable rule: a filter is a claim about coverage, and a claim about coverage needs a
two-sided check.** One side is what the filter selects; the other is what exists. Nothing in this
repository compared the two until a hidden abort forced someone to look.

## Refactoring candidates, derived from what actually broke today {#refactor-candidates-from-defects}

Compiled 2026-07-30 at the user's prompt, from the session's defect list rather than from taste. Each entry
names the defects that came out of it, so the case is evidence and not preference. Ranked by
value-per-risk, not by size.

**1. Absence must be expressible in the type. THE ONE TO DO NOW.** `CasRefCatalog::resolveLifeOrSentinel`
returns a `NamespaceLifeId` and, when the catalog does not name the namespace, returns the Stage-A sentinel.
So a caller **cannot distinguish "here is the life" from "I do not know"** — it receives a plausible,
well-formed, wrong key. That single property is the amplifier in all three vacuous-frontier findings
(`{#r11-empty-universe-vacuous}`, `{#r11b-authority-vs-union}`, `{#r11c-incarnation-mismatch}`): each one is
a proof fabricated out of a key space that is empty by construction.

Change the signature to `std::optional<NamespaceLifeId>` and delete the fallback; where a caller genuinely
wants the sentinel (raw test fixtures, where the sentinel IS the truth) it asks for it explicitly. **The
compiler then performs the sweep** — all 24 call sites must state what they do when the answer is unknown,
and R11c's class becomes a compile error instead of a silent proof. This is small, mechanical after the
signature change, and it PREVENTS the class we have now found three times.

**Mechanics, so whoever takes it does not re-derive them.** The signature change is one line; the work is the
24 decisions behind it, and they fall into exactly three shapes:
- **Raw test fixtures** — the sentinel IS the truth there, so they ask for it explicitly
  (`stageATransition(ns)` at the call site). Mechanical.
- **GC round paths** — must not fabricate: an unknown life means UNPROVEN plus an anomaly, never a probe at a
  guessed key. This is where R11, R11b and R11c all lived, and the fold now has `FoldResult::live_incarnation`
  to consult instead of re-resolving.
- **Diagnostic and administrative paths** (fsck, decommission, probe A, the admin rebuild) — an unknown life is
  a finding to REPORT, not a key to guess. fsck in particular must not emit a verdict computed at a guessed
  key, which is exactly the shape of the `crossEpochFromSeal` defect found in the checkpoint review.

**PLACED IN TASK 6 (2026-07-30), correcting the line that used to stand here.** I first wrote that this should
be its own task, on the grounds that 24 deliberate decisions must not be swept. That was half right: the
decisions must not be swept, but they are ALREADY Task 6's — deleting `stageATransition` forces every one of
its sites to say what it does when no catalog life is known. Doing it separately would touch the same sites
twice and create a merge surface against the task that has to touch them anyway. A mechanical sweep would
still re-create the fallback under a new name, so the mechanism (an `optional` return) is what forces the
decisions to be explicit. Attempted during Task 4-C and deliberately
deferred: the same files were in flight, and racing an active implementer across 24 call sites would have cost
more than the fix saves.

**2. One life resolution per round, threaded — not re-derived.** Five mechanisms answer one question, across
80 call sites: `resolveNamespaceLife` (10), `resolveLifeOrSentinel` (24), `discoverUniverse` (13),
`stageATransition` (19), `fromCatalogEntry` (14). Defects from this tangle alone: C2 (writers at the sentinel
while readers had moved to the real life), C3 (delete plan computed under one life, applied under another),
NEW-3 (an optional parameter defaulting to self-resolution at the one site its doc said it would not), and I3
(a full pool-wide catalog GET and decode per call, several in per-namespace loops, i.e. O(namespaces²) bytes
per round). Task 4-C started the fix inside the fold with `FoldResult::live_incarnation`; the same treatment
belongs in fsck and decommission, each resolving once per run.

**3. The destructive gate collapses per-namespace facts into a pool-wide boolean.**
`suppress_destructive = !anomalies.empty() || !holds.empty() || frontier_incomplete`. One namespace's anomaly
stops reclamation for the whole pool, which is how a single un-cataloged namespace becomes a **permanent**
pool-wide stall (`{#r11c-incarnation-mismatch}`'s neighbour, NEW-2). Task 7b's own text already requires the
flip to carry the hold set per namespace — so the gate wants to be per-namespace and is currently scalar, and
that mismatch has already produced one Critical and one stall hazard.

**4. `Gc/CasGc.cpp` (4679 lines) and `Pool/CasRefLedger.cpp` (4249) are 18% of the subsystem between them.**
Size here is not an aesthetic complaint: today's `chassert`-over-a-handled-branch sat four lines from the
branch it killed, the double-unlock that masked a real error class lived in the same file, and both survived
multiple reviews. **But NOT during open Criticals** — and when it happens, goldens first: this campaign's rule
is that an extraction needs its equivalence fences written BEFORE the move, because a fence added afterwards
tests the new shape rather than the preserved behaviour.

**5. The fixture/production divergence should be one named seam, not a habit.** Raw test helpers write at the
sentinel, admit catalog entries as `Live` with no `_ckpt`, and bypass birth — which produced the 164-test
sweep, the test whose premise was inverted by a uniform pin, and two tests that pinned data loss as correct.
One helper, one documented list of divergences, one place to look.

**6. And the one that is not a code refactor: keep converting prose rules into executing checks.** Four rules
failed today because they lived only in comments. The two that were converted — `FsckReport::clean` computed
from `kFsckHardFindings` with a `static_assert` in three TUs, and the suite-list generator deriving from
sources and failing on any unclaimed suite — both held immediately. Every remaining "whenever X, also do Y"
comment in this subsystem is a candidate.

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
