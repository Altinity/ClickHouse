---
description: 'Live backlog — garbage collection: scalability, byte cost, correctness follow-ups, and observability.'
sidebar_label: 'GC'
sidebar_position: 2
slug: /superpowers/cas/backlog/gc
title: 'CAS Backlog — GC'
doc_type: 'guide'
---

# CAS Backlog — GC {#gc}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for garbage-collection
scalability, byte cost, correctness follow-ups, and observability.

## GC scalability & byte cost {#gc-scalability}

- **[gc-frontier-one-list] Cheap change discovery: one LIST instead of per-namespace frontier GETs, plus a parallel walk** — DESIRABLE — USER-DIRECTED plan, 2026-08-03. The 2026-08-03 LIST-trust investigation is the settled-facts reference for this; do not re-argue the trust model without reviewing it first (absorbed into the correctness architecture page).

  Today every `Live`/`Removing` namespace costs the round at least one exact `GET cursor+1` (the frontier probe, the only thing that sets `frontier_proven`), even when `tail == cursor` says nothing changed, and the ref walk over `walk_targets` is a plain sequential `for` on the round thread (`Gc/CasGc.cpp:2134`) — so a quiet 10k-table pool pays ~10k serial GETs per round for discovery alone.

  Two independent levers:
  1. **Trusted-LIST frontier mode (per-backend, passported).** On a store whose LIST is certified compliant (lists started after a completed PUT include it), the round's single frozen LIST tail at `tail == cursor` IS the frontier proof, so the probe GET for quiet namespaces can be skipped. Scope limits that stay unconditional on every backend: the MIDDLE stays arithmetic (predecessor-omission below a correct tail is legal S3 behavior); the probe survives for HELD namespaces and for namespaces with no listed logs at all; `tail < cursor` still feeds the store-quality detector. Certification = the `Cas::Probe` LIST-consistency passport (`[LIST consistency on real S3]`, formats-and-storage.md). RustFS today FAILS the passport (proven-by-measurement omission, `2026-07-26-list-incompleteness-proof/`), so conservative (probe-always) stays the default and the mode is opt-in per backend, same shape as `[ckpt-read-policy]` (performance.md).
  2. **Parallel exact-key walk (backend-independent, no trust change).** Fan the per-namespace probes and the `cursor+1..tail` record GETs over a bounded pool (reuse/extend `meta_pool`); per-namespace work is independent by construction. Wall time for N quiet namespaces drops N× serial → N/K; worth it under lever 1 too, for the hot namespaces that still fold records.

  MEASUREMENT: per-phase timing rows exist (`d412f85f749`) — record the discovery phase's GET count and wall before/after each lever.

  Related orphaned findings folded in here (2026-08-04 triage): a proposed manifest cache to avoid
  redundant HEAD/GET on discovery (same discovery-cost class); a mount/lease introspection-package
  proposal, now absorbed by the landed `cas_mounts` system table; delta-runs + compaction follow-on
  work for the ack-floor round (same O(delta)-vs-O(universe) class); the Stage-A store-quality
  detector posture, which is exactly the probe-A/LIST-trust class the passport design above covers;
  and a repair-observability per-round cap mirroring probe A's cap — needs re-scoping since probe A
  itself was later deleted. Two more orphaned findings elaborate on the same trusted-LIST
  frontier item and restate its Lever 2 (parallel exact-key walk) verbatim.

- **[gc-snapshot-log-structured-runs] hot-pool snapshot rewrite is O(edges) per pass** — DESIRABLE — The single dominant remaining byte cost: a HOT pool rewrites the full snapshot run O(edges) per pass. Build O(delta)-write log-structured runs plus periodic compaction on the landed T2/T0 primitives (streaming reader, `seek`, `getStream`, ranged `get`, seal-ref resolution). Streaming reads and reference-parent runs (T2/T0) are already DONE; this is the remaining lever. (An orphaned 2026-08-04-triage finding states this same item verbatim — folded in as confirmation.)
- **[Lever B] Incremental point-updatable in-degree** — DESIRABLE — Makes even a non-idle small-delta round O(delta) (Lever A only short-circuits idle/no-destructive rounds). Also provides the global change-signal that would let GC drop the per-round `LIST(cas/refs/)` O(shards) sweep (gate #16 residue: discovery-placement quadratic is DONE; the O(universe)-per-round fold cost remains). Full-scale data: GC round is O(pool objects) — 87ms@400 parts → 93s@10k tables → 398s@100k parts. (An orphaned 2026-08-04-triage finding states this same item verbatim — folded in as confirmation.)
- **[ADAPTIVE-GC-CADENCE] journal-pressure-triggered fold** — DESIRABLE — Fold trigger should key on per-shard journal pressure (size/age), not changed-shard count, so one hot shard bounds deferral. Prod direction: modest `gc_interval_sec` (~30–60s) + journal-pressure trigger; constants pool-specific (soak-sweep the knee). Follow-up to Lever A. (An orphaned 2026-08-04-triage finding proposed the same cadence-tuning idea independently — folded in as confirmation, not new content.)
- **[distributed gc_shards>1 parallel GC] shard claim/scheduler** — DESIRABLE — Attempt-scoped generations are the prerequisite (DONE); the multi-worker shard claim + scheduler is not built.
- **[B148] HEAD storm at retire — stored-token optimization** — PARTIAL — The retire/recheck O(universe) HEAD phases are gone (v3 round); the residual condemn HEAD is bounded by newly-condemned candidates. Stored-token skip requires a manifest schema change (deferred). Related: **[PROMOTE-REVALIDATION-MINIMIZATION]** skip per-leaf promote HEADs when the installed round is unchanged since the dep observation.
- **[process_epoch → writer_epoch] stamp unification** — DESIRABLE — The writable path already sets `process_epoch = writer_epoch`; unify the manifest `writer_instance_id` stamps.
- **[PART-REMOVAL-REPOINT] part removal pays a wasted repoint of the doomed ref** — DESIRABLE (measured 2026-07-15 milestone soak) — 17 707 `ref_repoint` events, ALL on `delete_tmp_*` refs (writer node, mutations+TTL profile, 20 min): the rename-to-`delete_tmp` + per-file-unlink removal flow commits removal marks (a full stage+precommit+promote repoint, ≈3 PUTs) on a ref that the very next step removes entirely — ≈53K PUTs ≈ 22% of the writer's PUT class, pure overhead. The same-transaction `removeDirectory` supersede-clear (T8) already elides this when unlinks+rmdir share a txn; the cross-transaction removal flow misses it. Fix direction: defer/elide the marks-commit when directory removal follows, or widen the supersede window. (Contrast: scenario cards S03/S04/S05 assert `CasRefRepoint==0` and hold — this class only appears under part-removal churn.)
- **[GC-EMPTY-SHARD-PROBES] constant per-round 404 probe floor** — DESIRABLE (measured 2026-07-15) — ≈1 174 `DiskS3ReadRequestsErrors`/round, CONSTANT regardless of round work (work-driven HEAD/GETs all hit; the misses are the structural probe set of per-shard journal/run/seal keys that are absent for empty shards; grows ≈+4/round as the writer touches new shards). On a small/idle pool this is the dominant GC request class (~3.5K req/min at 3 rounds/min). Removed by [Lever B]'s change-signal (stop probing unchanged/empty shards); until then it belongs in the `07-s3-budget` request-count model (404s bill as requests).
- **[REF-QUEUE-WAIT-MEASURE] insert-path ref-lane queue wait ≈48 ms/insert** — DESIRABLE (measurement, 2026-07-15) — `CasRefQueueWaitMicroseconds` attributed to Insert queries = 339.6 s over 7 131 inserts (~48 ms avg) in the milestone soak; a data point for the refsnaplog Phase-2 flush-cadence/adaptive-threshold work — verify the batch-flush scheduling isn't leaving easy latency on the table before touching code.

## GC correctness / observability follow-ups {#gc-followups}

- **[GC round progress observability] round-duration watchdog + fold-window events** — HARD — A long/wedged round is only visible after the fact; emit a round-duration watchdog, LIST/fold-window progress events, and an alert on an unbalanced `gc_fold_begin`/`gc_fold_end` pair.
- **[ack-floor soak validation] 3 scenario cards** — TEST — SIGSTOP-a-writer holds-then-releases the floor; hard-KILL-writer → fence-out → fsck no-dangle; O(delta)+O(servers) request-count regression guard. Note: floor semantics changed under freshness-v3 (round-paced, per-hash `.meta`) — update the cards. Implemented + unit/TLA-covered; not soak-validated.
- **[F3] `ca-gc-dryrun` reachability under-counts vs real GC/fsck** — HARD — Systematic across S18/S25/S26/S33; real GC always safe (dangling=0). Fix: dryrun uses the SAME reachability walk as GC/fsck; assert `dryrun ⊆ (unreachable ∪ pending-gc)`.
- **[clamp liveness] scoped suppression under long persistent clamps** — DESIRABLE→HARD (2026-07-18: concrete reproducer) — Fail-closed clamp+suppression is correct and self-heals (false-404 attribution), but suppression-vs-liveness under long clamps is unaddressed (scoped suppression later). Clamp observability (clamped key/shard event) is DONE. The S38 sub-finding of 2026-07-18 — a poison late log clamps its own key and thereby starves `reportLateLogsIfAny` indefinitely (40 healthy Success rounds, sweep pass suppressed in every one, `RefLateLogDetected` never fires) — is MOOT as of Stage A task 6 (`d74c726ef9e`): the LIST-based late-ref-log detector it starved no longer exists, and a late log is now fenced by an in-band `EpochSeal` rather than reported after the fact (S38 asserts that fence directly). The general item stands on its own reproducers; the starvation shape is kept here only as the record of why a report-after-the-fact detector was the wrong shape. (Two orphaned 2026-08-04-triage findings cover this same S38 clamp-liveness gap, one noting the DESIRABLE→HARD escalation — folded in as confirmation, no new content.)
- **[gc-rebuild follow-ups]** — MINOR — Dedicated gc-round-log row for `rebuildBaseline` (currently only `LOG_INFO` + a `gc_rebuild` event); the "unowned-alive manifest edge over-protect" documented leak (bounded, fsck-visible, cleared by a future rebuild); soak validation (`mc rm gc/state` mid-soak → guard fires `CORRUPTED_DATA` → `SYSTEM … GC REBUILD` recovers to dangling=0).
- **[fsck oracle gaps]** — MINOR — fsck under-reports orphan manifest bodies for ref-less namespaces (enumerate `cas/manifests/` too, not just `cas/refs/`+`roots/`); fsck Orphan-class test gap.
- **[REBUILD R4 residual — manifest-less blobs unreclaimable]** — TRACKED, by design until R4 — Since Task 11 a rebuild condemns nothing (spec §7 — the zero-edge LIST/HEAD sweep was the r5-finding-4 data-loss vector), so a blob whose manifest no longer exists anywhere in the pool has no row in the rebuilt baseline and the incremental pipeline can never reach it. Such blobs are RETAINED and show as fsck `unaccounted` that does not drain after a disaster rebuild. This is the NAMED staging-contract residual of register R4 (the build/upload registry, which is what can enumerate in-flight uploads safely). NOT a bug and explicitly NOT to be closed with a substitute reclamation: any rule that reclaims from an enumeration reintroduces the same vector. Closes when R4 lands.
- **[repointRef non-resolving-key audit gap]** — MINOR — `CachedPartFolderAccess::repointRef` (`CachedPartFolderAccess.cpp:283`) increments `CasRefRepoint` and logs "Repointed committed ref…" unconditionally after its `if (resolved)` byte-equal check, even when `resolve(key, ForceFresh)` returns `nullopt` — i.e. it would count/log a repoint for a key with no existing committed ref. Unreachable today (every caller — Task 4's standalone writes, Task 8's removal-mark resolution — only calls `repointRef` on an already-resolving key); a defensive `throw LOGICAL_ERROR` on `!resolved` would make the precondition explicit and the counter/log trustworthy rather than merely-currently-true. (Found during all-tree Tasks 7/8 integration review.)
- **[ProvenanceOp operability gap]** — MINOR — Both the Task 4 committed-ref standalone write and the Task 8 removal-mark repoint call `repointRef(..., Cas::ProvenanceOp::Other)` — no distinct op kind for a removal-repoint vs a write-repoint in `system.content_addressed_log`. Spec doesn't require one; would help an operator distinguish "this repoint dropped files" from "this repoint added/changed files" in the audit trail without decoding the entry diff. Product-owner call, not decided during Task 8 integration. (Found during all-tree Task 8 integration review.)
- **[codex-11] namespace drop misses an unregistered build → ownerless Live namespace** — LOW — `Pool::beginPartWrite`'s allocate/register window (`CasPool.cpp:772-777`) is real: a build can allocate before the drop sweep (which only snapshots `inflight_builds`) runs, then legitimately pass the birth-time marker gate afterwards, reviving a Live-but-ownerless EMPTY ref-table — a small, non-self-healing metadata leak (GC never sweeps Live namespaces). Confirmed narrow, LOW severity (2026-07-17 codex-review triage, finding №11); the reviewer's atomic-registration fix would NOT close it (the same TOCTOU recurs between the `cancelled` check and the append for already-registered builds). Fix direction: a GC backstop that reclaims empty ownerless Live namespaces, or a namespace generation folded into the birth-time marker gate.
- **[RECOVERED-INDEGREE-ATTRIBUTION] move the "delete_pending recovered in-degree" invariant check to the writer** — DESIRABLE — The GC-side `LOG_WARNING` at `CasBlobInDegree.cpp:418`/`CasGc.cpp:485` fires as a false alarm (56x/run on tiny system-log blobs): root-caused to a dedup-adopt-vs-condemn TOCTOU where `observeAndAdmit` (`CasPartWriteTxn.cpp:354-421`) adopts a token just before GC condemns it, and GC correctly spares it (no data loss). Fix: downgrade the GC log to a `ProfileEvent` + `LOG_DEBUG`, and add a real detector at the writer's edge-commit (typed `BlobAdoptRacedCondemn` event) so the false alarm goes away without losing the one genuine bug class it masks. RCA in `project_pr2073_ci_triage_2026_07_23`.
- **[CONDEMN-GRACE-WINDOW] cool-down before condemning a just-zeroed blob** — DESIRABLE — The driver of the RECOVERED-INDEGREE noise: tiny system-log blobs whose in-degree hits 0 and is re-referenced by dedup almost immediately. A short grace/cool-down before condemning a blob whose in-degree just transitioned to 0 would remove the churn, but this changes condemn timing and touches GC invariants (retention vs. reclaim latency, ack-floor interaction) — needs TLA-level reasoning and is subject to the protocol-step-change veto, not a "cheap" tweak. Measure first: count re-condemn/HEAD/spare cycles on the 28 hot hashes from run 30019911967 to size the benefit.
- **[REBUILD-SEAL-POINT-READ] point-read closure for REBUILD seal discovery** — HARD — `rebuildBaseline` finds the pool's newest fold seal via a bounded probe-and-step-down over `gc/gen/<G>/` listings (probe-A-style detection, not proof) because there is no dense point-readable attempt id to walk. Two residuals: (1) a store that lies about one generation's own prefix can still hide it; (2) the "virgin pool, no holds" verdict can be silently wrong on a PRUNED pool (`CasGcRebuildVirginByEnumeration`, visible but not prevented). Fix: a write-once `gc/gen/<G>/sealed` alias minted at adoption makes discovery a dense exact-`GET` walk, closing residual (1); residual (2) needs something that survives pruning (an unpruned marker location, or a pool-level floor pointer) — a small protocol/format amendment, deferred to the spec-amendment decision. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-8-report.md`.
- **[STAGE-B-7B-SEQUENCING] ✅ SATISFIED — the incarnation-keyed closure landed BEFORE the flip (verified 2031-triage side-check, 2026-08-21); kept for provenance** — {#stage-b-7b-sequencing} — was a HARD CONSTRAINT — `bf396ffa50d` (2026-08-01) re-keyed the fold-seal cursor map from `map<String, ShardCoverage>` ("ns/shard", BY NAME — exactly the shape this constraint warned about) to `map<UInt128, RefLifeFoldState> ref_lives` keyed by catalog incarnation (`Formats/CasFoldSealFormat.h:160-168`, `:104-108`); `58fd482a800` flipped `kDefault = Authoritative` two days later. Verified in code: universe comes from the catalog only (`discoverUniverse`), the admission loop is keyed by `entry.incarnation` with unmatched parent rows dropped (`Gc/CasGc.cpp:228-259`), walk targets come from `live_incarnation` (`:2108-2135`), a LIST hint is admitted only when its physical life id equals the current incarnation (`:1735-1744`), `_ckpt` keys are minted per-life (`:1408-1419`), ref-log keys are physically incarnation-qualified (`Formats/CasLayout.h:133-161`), and every no-proof exit is fail-closed (`:2202-2214`, `:2724-2726`, `:2755`, `:2774`). The commit's "UNVERIFIED-DRAFT" marker is stale — the flip is 564 commits behind a CI'd sha. Remaining debt is prose only: recompute {#suppressed-handoff-consumption} (its "stops being systematic when kDefault flips" premise has now happened) and correct the stale "hint ∪ sealed cursors ∪ catalog entries" universe comment at `Gc/CasGc.cpp:2877-2878`. ORIGINAL TEXT: Stage B's Task 7b (`kDefault = StageA_Suppressed` → frontier-consulting) must not happen first: Task 9's destructive-gate universe union means a namespace removed and recreated inside one writer epoch can restart its ref-txn ids at or below the retained cursor, so the walk can miss the recreated edges entirely — live blobs would look unreferenced. Contained today because Stage A destroys nothing; the containment evaporates the instant 7b flips. Structural closure: cursors keyed by `(namespace, incarnation)` rather than by name. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-9-report.md` §12.
- **[SUPPRESSED-HANDOFF-CONSUMPTION] a suppressed round consumes the hand-off reclaim instead of deferring it** — {#suppressed-handoff-consumption} — MINOR (bounded leak, fsck-visible) — The post-CAS hand-off (`handoff_reclaim` phase) reclaims a generation the wholesale prune skipped; under Stage A's blanket suppression this DROPS rather than defers, and nothing revisits it — left to `fsck`, same class as a pre-existing crash-window leak, just more frequent. Stops being systematic the moment `{#stage-b-7b-sequencing}` flips `kDefault`. Pinned by `CasGcFrontierGate.TheHandOffReclaimIsInertUnderSuppression`.
- **[FSCK-SCALE-TIMEOUT] `ca-fsck` cannot complete a large pool within its own deadline** — {#fsck-scale-timeout} — MEASURED — At ~29-31 GiB the audit times out (`FSCK_EXIT=159`) and returns nothing at all — "fsck clean" is unmeasurable exactly where an operator wants it most; raising the harness budget 180→600s did not help (the cost is the pool, not the budget). Direction: bounded/streamed partial verdicts (per-namespace pagination with a resumable cursor) and deadline-aware partial reporting via the existing `partial`/`partial_reason` fields. Structural consequence: a phase-3 soak's fsck-clean gate stays reported UNARMED under Stage A at any pool-growing workload; complete audits at auditable scale (05020 + scenario end-checkpoints) remain the real evidence. Related: soak-fsck-checkpoint-budget (testing-and-ci.md), gc-scalability (this file).
- **[CA-LOG-TABLES-RESTART-COST] the CA instrumentation's Outdated churn failed the restart health gate** — {#ca-log-tables-restart-cost} — NEW (gc-audit 2026-07-29) — the 6/40 soak's post-kill restart took 178.9 s against a 180 s gate: 40.0 s CAS mount-lease token-stability wait + 138.1 s reloading SYSTEM-LOG tables' Outdated parts (`system.content_addressed_log` alone: 299 Outdated parts; the USER table loaded 1 part in 15 ms). The observability that makes a soak a specimen is what failed its checkpoint. Directions: TTL/partitioning for the CA log tables, bounded event-log part churn, lazy system-log load. Related: the merge-churn datum (generations 74k+/35 min).
- **[CKPT-DAMAGE-NO-REPAIR-PATH] a damaged `_ckpt` has no repair path and still shuts the round-wide destructive gate** — {#ckpt-damage-no-repair-path} — The fix (`e337bb2c87d`) converts an undecodable `_ckpt` from a pool-wide FOLDING halt into a per-namespace hold (`BodyUndecodable` precedent, cursor rides verbatim, other namespaces fold; WARNING names namespace+key) — but TWO residuals stand: (a) a held namespace still shuts the ROUND-WIDE destructive gate, so one unrepaired `_ckpt` stops all reclamation pool-wide until repaired — full isolation needs Stage B's per-namespace destructive gate (the comment at `CasGc.cpp:2515` names the set the flip must carry); (b) there is NO repair path — `publishCkpt` read-merge-CASes the same object and hits the same decode failure, so the namespace's own writer is stuck too; candidate fixes = `fsck --repair` recreating from the fold contribution, or a `publishCkpt` recreate-on-undecodable arm (PROTOCOL-ADJACENT — user consult; fail-close question: recreating a `_ckpt` must never lower the frontier proof). Until then the operator action for a damaged `_ckpt` is manual object surgery.
- **[GC-DEFER-DECISION-LIST-COST] the round's whether-to-fold decision costs a full pool LIST — 79% of all GC time** — {#gc-defer-decision-list-cost} — TRACK-B HEADLINE — With rounds now bounded, the leader is still busy 90.5% of wall, and 79.11% of it is `defer_decision`: a full ~177k-key LIST every round, sometimes concluding `changed_shards:0` after 127s (the idle-round control case costs 5 orders of magnitude less). The frozen-tail design requires a LIST to discover tails, so any fix is design work: candidates are a scoped per-namespace LIST with start-after markers, tail discovery from the previous round's coverage plus delta probing, or LIST-page caching keyed by namespace tail. PROTOCOL-ADJACENT — user consult before any change to what the round reads.
- **[GC-FULL-TIME-ACCOUNTING] every millisecond of a GC round must be attributed** — {#round-duration-alarm} — TRACK-B ITEM — Two measured blindnesses fixed the visibility gap: a never-completing round used to be invisible (no Finish row), and completed rounds showed minutes living outside every timed phase span. Timer coverage is now measured 99.986% complete (0.9s unaccounted out of 6,443.4s), with the three remaining un-timed spans identified and all sub-20ms. Remaining work: name the `orphan_sweep` epilogue phase, add an `unaccounted_ms` self-check column to the Finish row, and add a periodic in-round progress log line past a sane elapsed bound.
- **[gc-checkpoint-timeout-tsan] ca-soak GC-checkpoint timeout formula assumes normal-speed GC throughput — blows the budget under TSan** — MINOR (GREEN-DEBT) — `soak/checker.py:fixpoint_timeout_s` assumes a normal-speed GC reclaim rate; under TSan's instrumentation overhead a genuinely-converging backlog (confirmed trending down, `dangling=0` throughout) can still blow the budget — a harness-timing artifact, not a correctness bug. Fix: add a sanitizer-aware multiplier to `fixpoint_timeout_s` (or an explicit CLI override); low priority since the gtest battery and other soak stages already validate TSan correctness.
- **[ckpt-neverborn-gc-backstop] GC-level backstop needed for never-born `_ckpt` debris** — {#ckpt-neverborn-gc-backstop} — HARD — Task 4-C removed all three `cleanupOrphanedBirthCkptBestEffort` call sites: each sat on the `CORRUPTED_DATA` path, which by `putIfAbsentControlled`'s own contract means the ref-log at that key is non-empty — contradicting the delete's own "never durably held anything" justification, so a successor's live `_ckpt` could be deleted unrecoverably. Correctly removed rather than guarded (a fallback must never take a destructive action). Traded for: permanent debris that makes a drained server root refuse decommission (`claimOwnerOrThrow` → `CORRUPTED_DATA`), operator-visible. The needed backstop, mirroring the existing REMOVED-namespace `_ckpt` backstop in `CasGc.cpp`: reclaim a never-born namespace's `_ckpt` only after independently re-verifying emptiness with a real LIST, never by inferring it from one attempt's own conflict.

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
  `remote-data-paths-no-pushdown` (testing-and-ci.md); amplified but did not cause this collapse.

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
`(M-F debris, B140)` mislabel for this population is tracked in `fsck-large-pool-fixed`
(operability-and-introspection.md).

## The adopted fold seal referenced a PRUNED generation's run (observed 2026-07-25, same stand) {#adopted-seal-pruned-run-2026-07-25}

Once observed: an adopted fold seal's `blob_target_runs` named a generation's run object that
`pruneSupersededGenerations` had already reclaimed, despite `CasGc.cpp:629-645`'s explicit
"retention must never reclaim these" guard over the parent seal's referenced generations. While it
lasts, `zeroInDegree` sees zero candidates from the absent run and the round silently reclaims
nothing (no throw on 404 per the GC-never-throw-on-404 invariant). NOT ESTABLISHED: whether the
prune raced the adopt, or the parent-seal capture missed the carry. Needs a targeted test, not
log archaeology.

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[gc-checkpoint-record-tuning] resident-snapshot incremental GC checkpoint tuning** — DESIRABLE — `gc_checkpoint_records`/`gc_checkpoint_rounds` config-level tuning for the resident-snapshot incremental checkpoint is a distinct configuration-level detail not covered by the scalability items above.
- **[gc-silent-frontier-exit] T6a's silent-frontier-exit enumeration gap** — HARD — A walk-that-never-started shape: a genuine observability/correctness gap in GC frontier proof coverage, found during T6a verification.
- **[gc-handoff-single-crash-leak] single-crash leak window in the GC round hand-off (Task-7)** — MINOR — A concrete, bounded leak class in the post-CAS hand-off; plausibly worth a tracked item on its own, distinct from the suppressed-handoff-consumption item above (that one is about suppression under Stage A, this is a single-crash window).
- **[retired-refs-map-staleness] `retired_refs` map staleness after retired-in-snapshot** — MINOR — Needs live-field verification before removal; a concrete cleanup task once verified.
- **[r11c-incarnation-mismatch-detector] R11c incarnation-mismatch detector design** — DESIRABLE — A distinct anomaly-detection gap from the settled R11 vacuous-universe finding; a detector design, not yet built.
- **[removing-cursor-write-twice] `Removing`→`RemovalReady` cursor-write-twice + final-deletion revalidation edge case** — VERIFY — A concrete correctness question surfaced by a strategic review round; not yet independently verified against HEAD.
- **[namespace-removal-ordering-cost] namespace-removal ordering (`_ckpt` exact-delete while `Removing`, catalog-entry CAS-delete last) + catalog size-bound cost** — VERIFY — A concrete correctness/cost detail from a review round; verify the stated ordering still holds and size the catalog cost.
- **[gc-probe-a-counters-durability] process-local GC probe-A counters vs durable `gc_anomaly` audit rows** — VERIFY — A concrete observability-durability gap: probe A itself was later deleted (Task T5), so this needs re-scoping — either fold into `gc_anomaly` durability if still relevant, or close as moot.
- **[frontier-attribution-taxonomy] frontier-attribution classification taxonomy (6 exhaustive classes) for unproven namespaces** — DESIRABLE — A concrete diagnostic-tooling gap: no classification exists today for why a namespace's frontier proof is missing.
- **[orphan-sweep-byte-budget] orphan-manifest nomination is object-count-bounded, not byte-bounded** — DESIRABLE — `nomination_budget` (`CasOrphanManifestSweep.cpp:605-638`) caps candidate count, not bytes; with 256 MiB manifests the retained-byte axis can still reach ~25 GiB per round.
- **[stateless-reader-mark-fence-blind-spot] a ref-less catalog-only reader is invisible to the `use_count()==1` mark-union fence** — DESIRABLE — The `use_count()==1` gated candidate loop is confirmed live (`Pool/CasRefLedger.h:804`, `.cpp:1618`); verify whether a stateless/ref-less reader can race a concurrent GC decision — if real, needs an explicit registration or a documented safety argument.
- **[gc-files-prefix-not-listed] verify `_files` debris is reclaimed without a GC LIST of `rootsPrefix()`** — DESIRABLE — `Cas::Gc`'s fold LISTs `casRefsPrefix()` but not `rootsPrefix()`, so `_files` keys may not reach a GC round even though the writer's `removeRecursive` handles both prefixes. Confirm the perpetual namespace janitor (not the fold) is the actual reclaimer for this class, and document it if so; if not, it's a leak.
- **[decode-cache-ttl-vs-gc-graduation-assert] assert `shard_decode_cache_ttl_ms` stays below GC's condemn-to-graduate window** — DESIRABLE — No assertion enforces `shard_decode_cache_ttl_ms` (200 ms) staying below the condemn-to-graduate latency (minimum two full rounds), which is the actual safety margin this cache setting depends on. A future GC-latency tuning change could silently violate this margin with no test catching it.
- **[gc-rebuild-lease-interlock] `cas-gc-rebuild`'s `rebuildBaseline` has no mount-lease interlock** — HARD — A live server's fresh mount lease does not stop the offline disaster-recovery rebuild from performing; the only live-server guard today is the caller's own read-only-open discipline. Real safety gap in a destructive tool.

## Orphan sweep's no-catalog-row branch skips every gate; its premise is false (2031-triage CAS-022) {#orphan-sweep-absent-catalog-row-window}

Two paths differ. The addressed path `sweepNamespace` correctly refuses without a catalog row
(`Gc/CasOrphanManifestSweep.cpp:499-500,518-520`) — the audit's claim is false there. But the paged
planner `planManifestCursorPage` conditions ALL of its gates on `catalog_entry` (`:700-711`
watermark, `:740`, `:751`, `:817`, `:828-830`) and nominates directly at `:875-895`, so a manifest
whose namespace has no row yet passes with no watermark, no coverage and no §6 premise.

The comment at `:822-826` justifying this states that a catalog row is published before any object
of the life — that is wrong at HEAD: `stageManifest` writes the manifest BODY
(`Pool/CasPartWriteTxn.cpp:809-878`) while the `Creating` row appears only later inside
`precommitAdd` → `appendRefOps` → `createNamespace` (`Pool/CasRefCatalog.cpp:579-590`). So a
namespace's first write does have a window.

Mitigations keep this out of data-loss class: `promote` is fail-closed on a missing body
(`CasPartWriteTxn.cpp:1032-1039`) so the INSERT fails loudly rather than committing a hole, and
`BlobSourceRetirement` is idempotent (`Gc/CasBlobInDegree.h:167-174`). Fix: make the paged planner
require the same durable premise as `sweepNamespace` (no row ⇒ skip, not nominate), and correct the
stale ordering comment. P2.

## Janitor page size is hardcoded and pages the whole namespace prefix (2031-triage CAS-034) {#janitor-page-hardcoded}

The namespace janitor's page is a hardcoded 1000 keys (`Gc/CasGc.cpp:470`) with no setting, and it
pages over the whole `namespaceRootPrefix()` rather than over the debris it means to reclaim — so
post-`DROP` erase latency is O(all namespace objects), not O(debris). Erase SLA itself is not part of
the disk contract (settled position: the operator can `GC RUN` at any time), which is why this is
latency, not loss; but the hardcoding and the prefix width are both fixable: make the page a setting
and scope the LIST to the removed namespace's own prefix.

Corrected premise while checking CAS-034: ref mutations batch into ONE `_log` object per flush
(`Pool/CasRefLedger.cpp:2615`, `:3148`), so the ref-object creation rate is latency-bounded, not
two-objects-per-part-commit as the audit assumed.

## GC fold materializes a whole shard edge-run twice in memory (2031-triage CAS-035) {#fold-edge-run-memory}

The round's real peak-memory site is `foldDeltasIntoGeneration`'s `WriteBufferFromOwnString` plus
`out.str()` (`Gc/CasBlobInDegree.cpp:389`, `:678-681`) — two full copies of the entire shard edge-run
in memory, and `gc_shards=1` by default means the whole pool. The input side is already streamed, so
this is the one unstreamed hop. Belongs to the tracked O(pool)-per-round class ({#gc-scalability}
`[Lever B]`, `[gc-snapshot-log-structured-runs]`, `[gc-frontier-one-list]`); recorded here because the
existing items name the LIST, not this materialization. Note the enumeration itself cannot simply be
skipped on deferred rounds — the defer signal is computed FROM it (`Gc/CasGc.cpp:628-646`), which is
what `[Lever B]`'s change-signal is for.

## `SYSTEM CAS GC REBUILD` never says WHY it judged `gc/state` unhealthy (2031-triage CAS-069) {#rebuild-gcstate-decode-reason-unreported}

`Gc::rebuildBaseline`'s health probe decodes `gc/state` inside `try { decoded = decodeGcState(...) }
catch (...) { /* undecodable state = scenario (а) */ }` (`Gc/CasGc.cpp:3874-3883`) and then treats
`!decoded` as scenario (а) — the disaster the command exists for. The safety half is sound and must not
be changed: the undecodable branch does not hand back a hold-free baseline, it re-discovers the newest
fold seal by enumeration and REFUSES with `CORRUPTED_DATA` when that seal is undecodable or vanished
(`:3955-3990`), so the only hold-free outcome is a pool with no seal object anywhere, reported on the
command's own row (`rep.virgin_by_enumeration`, `:3963`). The gap is purely diagnostic: the discarded
exception is the only evidence of WHY the state did not decode, so an operator cannot distinguish real
byte damage from an environmental failure of the decode itself, and the `RebuildReport` carries no field
for it either. Owed: log the caught exception (as the janitor does with `e.what()` at `:485` and the
condemn-marker re-check does with `tryLogCurrentException` at `:1897`) and name the reason on the
report row. Peer
items: `BACKLOG.md`{#damaged-object-repair} item 1 asks fsck for exactly this present-and-undecodable
vs absent distinction on the same object kinds; `{#gc-followups}` `[gc-rebuild follow-ups]` already owes
the rebuild a dedicated gc-round-log row, which is where the reason belongs. Not a defect, checked in
the same pass: the `std::stoull` empty catches under `gc/gen/` (`:1490`, `:1621`, `:4096`) cannot
misclassify a transient failure — `stoull` fails only deterministically — and cannot skip a well-formed
generation key, so `max_gen` is not under-computed by them.

## A dead member whose slot is never reclaimed keeps its abandoned build prefixes ineligible forever (2031-triage CAS-077) {#dead-member-frozen-build-floor}

`prefixEligible` derives eligibility ONLY from the victim's own mount lease
(`Gc/CasOrphanManifestSweep.cpp:476-493` via `floorForNamespace`, `:45-64`): old epoch ⇒ eligible,
same epoch ⇒ eligible only when `min_active > build_sequence`, `min_active == UINT64_MAX` (farewell)
⇒ everything eligible. Node loss does NOT delete the mount object, so the floor is not *missing* — it
is FROZEN at the last heartbeat: `min_active` never advances again, and neither GC's fence-out nor the
`gc_fenced` flag it stamps touch `writer_epoch`/`min_active`. So for a member that dies and whose slot
is never reclaimed (no same-`srid` restart, no successor claim — both bump `writer_epoch` and drain the
old epoch — and no `SYSTEM CAS DROP POOL MEMBER`), the builds that were in flight at the moment of loss
stay ineligible indefinitely: their manifest bodies AND the blobs those manifests pin (the fold treats a
not-eligible unowned manifest as a live edge — `Gc/CasGc.cpp:4290-4297`). Retention, not loss, and
bounded by the in-flight build concurrency at the moment of loss — but the bytes can be part-sized, and
the only reclaimer left is the decommission verb, which by design erases the member's namespaces first
(see {#owner-only-slot-invisible-in-mounts} / CAS-063's "data first, then slot" invariant).

Deliberate posture, not an oversight: control #9 (`CASOrphanManifestSweep.NoWatermarkIsNotAuthority`)
exists precisely to forbid replacing the durable floor with a frozen-sequence or judged-dead guess, and
`gc_fenced` is a weaker certificate than it looks for THIS purpose — a fenced predecessor's in-flight
PUT can still materialize a manifest body afterwards (`BACKLOG/ref-protocol.md` `[Late Predecessor
PUT]`), so promoting "fenced" to "every seq of this epoch is retired" needs the same landing analysis,
not a one-line change.

Owed (cheap half first): (1) observability — a fenced/never-reclaimed slot's retained prefixes are
invisible; `sweepNamespace` returns 0 with no retain class (they fall into the undifferentiated
`skipped` tally, `Gc/CasGc.cpp` `reportSweepRetention`), and `ca-fsck` labels them
`in-flight-pre-precommit` (`Tools/CasFsck.cpp:1113-1116`) — a lie for a member that is provably gone.
Owed: a distinct retain/report class ("stranded behind a dead member's frozen build floor") naming the
`srid`, so the operator sees the retained bytes and knows the remedy. (2) the reclaim decision proper —
either a non-destructive administrative verb that stamps only the farewell sentinel on a proven-dead
slot (leaving the namespaces), or an explicit statement that decommission is the only answer. PROTOCOL
ADJACENT (it licenses deletes on a dead member's behalf) — user consult before any change.

## The namespace janitor rewinds its durable cursor on ANY LIST failure (2031-triage CAS-078) {#janitor-cursor-rewind-on-list-error}

`NamespaceJanitor::runOnePage` wraps its single LIST in `catch (...) { (void)casGcMaintenanceState(...,
GcMaintenanceState{}); throw; }` (`Gc/CasNamespaceJanitor.cpp:22-31`), so a transient S3 5xx / throttle /
timeout publishes an EMPTY cursor and the next round restarts the ownership-tree enumeration from the
beginning. The reset is deliberate and pinned
(`gtest_cas_namespace_janitor.cpp:548-561`, `CASNamespaceJanitor.BackendRejectedCursorResetsExactlyAndDeletesNothing`),
but the code cannot tell a rejected cursor from an ordinary transient failure — and the cursor is not an
opaque expiring continuation token, it is the last returned key, resumed with `start_after`
(`Backend/CasBackend.h:124-129`, `:260-262`), so a transient failure never invalidates it. Combined with
the one-1000-key-page-per-round pacing ({#janitor-page-hardcoded}, `Gc/CasGc.cpp:470`, one call per round
at `:710`/`:1221`) a backend with a per-LIST failure probability comparable to the page rate can keep the
janitor pinned to the head of the prefix, so dead-life debris deeper in the tree is never reached. Not
loss and not silent: the exception is logged (`Gc/CasGc.cpp:485-488`) and the residue is counted by fsck
(`Tools/CasFsck.h:155-157` `namespace_janitor_pending*`); erase latency is not part of the disk contract
(settled position under {#janitor-page-hardcoded}). Owed: keep the cursor on a transient failure — reset
only for a cursor the backend genuinely refuses (deterministic invalid-argument class), or only after N
consecutive failures at the same cursor. P3.

## GC ref-object trimming still gates on whole-catalog token stillness (2031-triage CAS-079) {#ref-cleanup-whole-catalog-token-stillness}

`Gc::cleanupRefObjects`'s per-key revalidation refuses when `current_catalog.token !=
folded.catalog_cut->token` (`Gc/CasGc.cpp:3424`) even though the same condition already compares THIS
namespace's row by value and re-resolves its life (`:3425-3428`); the ref catalog is one pool-global
object ({#ref-catalog-write-hotspot}), so any `CREATE`/`DROP`/state transition of any table in the pool
during the (long, O(pool)) window between the fold's catalog read and post-CAS cleanup refuses the
delete — and the refusal `return`s out of the WHOLE function (`:3506`, `:3518`), abandoning the remaining
namespaces' trimming too, not just the affected one. This is the exact class `684161dcc03` ("prove
namespace absence per-row, not by whole-catalog stillness") removed from the ref-writer presence probe
and cold-reader admission after `01069_database_memory` failed 193 of 194 retries on it; the GC cleanup
site was not part of that commit and is pinned to the old contract by
`CASRefGcCleanupAuthority.CatalogTokenMoveBeforeFirstDeleteRefusesEveryRefObjectDelete`
(`gtest_cas_ref_gc.cpp:563-579`), whose injected move rewrites the catalog with BYTE-IDENTICAL content
(`:159-176`) — i.e. it pins the over-sensitivity itself. Bounded to reclaim latency, not loss:
`planRefCleanup` recomputes the same candidates from durable state next round (`Gc/CasGc.cpp:3497-3501`).
Owed: per-row revalidation as in the ledger fix (row by value + life resolution + explicit
`throwIfAmbiguous`, which is what catches the aliasing incarnation the token compare used to catch), the
refusal scoped to its own namespace rather than the whole pass, and the two pinning tests re-aimed at the
per-row contract. P2.

## A stranded generation prefix is reclaimed by nothing AND enumerated by nothing — the "fsck is the backstop" claim is false (2031-triage CAS-074) {#stranded-generation-prefix-invisible-to-fsck}

Three sites promise that a generation prefix the wholesale prune skipped and the one-shot hand-off then
failed to reclaim (suppressed round, crash between the round CAS and `handoff_reclaim`, or hand-off budget
exhaustion) is "left to `fsck`, which is the backstop": `Gc/CasGc.cpp:1105-1107`, `:1126-1128`,
`:1144-1149`, and this file's own {#suppressed-handoff-consumption} ("bounded leak, fsck-visible").
`runFsck` never enumerates `gc/` at all: its only listings are `layout.blobsPrefix()`
(`Tools/CasFsck.cpp:728`) and the manifest prefixes (`:914`, `:1099`), and the only `gc/` keys it reads are
the CURRENT `gc/state` and the CURRENT fold seal by exact key (`:817`, `:833`). So a stranded
`gc/gen/<g>/` prefix appears in no counter, no `FsckObject` row, and no soak residual metric — it is
neither reclaimable (`snap_pruned_through` is monotone and `pruneSupersededGenerations` only walks forward
from `next.snap_pruned_through + 1`, `Gc/CasGc.cpp:3621`; `rebuildBaseline` carries the cursor over
unchanged) nor observable. Magnitude is also understated: the prefix holds that generation's snapshot RUN
objects, which are `O(edges)` in a hot pool ({#gc-snapshot-log-structured-runs}), not "one small run per
shard". Owed, cheapest first: (a) correct the three prose claims and the {#suppressed-handoff-consumption}
"fsck-visible" wording, since a false backstop is worse than a named leak; (b) give fsck a bounded
`gc/gen/` enumeration that reports prefixes strictly below `snap_pruned_through` as an advisory count, so
the class stops being invisible; (c) only then consider a reclaimer (a prune pass keyed on that advisory,
not a lowering of the monotone cursor). P2 — bounded, no correctness or data-loss dimension.

Same-shape false prose found alongside it, unrelated to the hand-off: `Pool/CasServerRoot.h:768-771` and
`src/Disks/tests/gtest_cas_s3_staging.cpp:895-897` both state that "GC blob discovery LISTs
`Layout::blobsPrefix()`" as the reason `staging/` is safe from GC. GC lists nothing under `blobs/` at HEAD
— it only HEADs and deletes keys derived from edges (`Gc/CasGc.cpp:802`, `:817`, `:1820`, `:4437`); the
per-round blob LIST was deliberately removed (the GC-DISCOVERY-LIST-QUADRATIC concern, `:3665-3680`). The
staging-separation conclusion still holds for a strictly stronger reason (GC cannot reach a key no edge
names), so this is prose only — but the test named
`CASS3Staging.GcBlobDiscoveryPrefixExcludesStagingObjects` pins a premise that no longer exists. P3.

## A refused `GC REBUILD` is not side-effect free, and its own header says it is (2031-triage CAS-094) {#rebuild-refusal-leaves-run-and-seal-residue}

`Gc::rebuildBaseline`'s LAST refusal — the state CAS lost to a competing writer
(`Gc/CasGc.cpp:4377-4381`) — happens AFTER the unconditional per-shard `flush_shard` loop
(`:4335-4336`, each call a `foldDeltasIntoGeneration` that PUTs run objects) and AFTER the fold seal
itself (`putDeterministicArtifact`, `:4366`). So every lost-CAS refusal returns `performed == false`
while leaving a complete, `validateFoldSealForWrite`-passing seal plus its whole run set durable at
generation `max_gen + 1` (`:4102`). The earlier data-loss refusal (`:4209`, a committed ref naming a
missing manifest) can also land after run PUTs, but only on a pool that overflowed
`rebuild_edge_budget` (default 8 000 000 edges per shard, `Pool/CasPool.h:167`) in an earlier
namespace; the two health refusals (`:3997`, `:4074`) and the lease refusal (`:4012`) precede every
`gc/`-plane write.

What the residue does NOT do, contrary to the finding as filed: it is not adoptable by a later regular
round. A round reads the baseline only through `gc/state`'s `snap_generation`/`snap_attempt`
(`listRefPrefix` `:2871`-ish `readFoldSeal`, `graduationDue`), and the refused CAS left that pointer
untouched. The only reader of an unadopted seal is a LATER rebuild's `newestFoldSealRef` (`:1464`),
and carrying an unadopted attempt's holds is the deliberate, documented over-hold (`:3941-3957`), not
a loss. The residue is also not a permanent leak: it sits at `snap_generation + 1`, which the
wholesale generation prune reaches once the adopted generation advances past it plus `keep`
(`:3618-3662`) — unlike {#stranded-generation-prefix-invisible-to-fsck}, whose prefixes sit BELOW the
monotone cursor.

What is genuinely open, all of it minor:

1. **Prose that is false at HEAD.** `ContentAddressedMetadataStorage.h:199-200` states "A refused
   rebuild (`report.performed == false`) writes nothing". The lost-CAS refusal always writes runs and a
   seal. Fix the comment (cheapest, and a false claim about a disaster-recovery tool is the worst kind).
2. **The refusal is unreported.** Only the success path emits a `GcRebuild` event (`:4386-4404`); each
   refusal `return`s a string with no event and no log, so nothing in
   `system.content_addressed_log` records that a generation's worth of run objects and a seal were
   minted and abandoned. Pairs with `{#gc-followups}` `[gc-rebuild follow-ups]`, which already owes the
   rebuild a dedicated gc-round-log row.
3. **The rebuild's attempt numbering is not lease-derived.** A regular round stamps
   `attempt = state.lease.seq` (`:1921`) while the rebuild counts `++attempt_of[shard]` from 1
   (`:4121`). Both write under `gcGenAttemptPrefix`, and the rebuild's generation `max_gen + 1` is
   exactly the generation the next regular round mints (`snap_generation + 1`, `:1915`). A collision on
   `(generation, attempt, shard, seq)` therefore needs a later round whose `lease.seq` is ≤ the
   rebuild's flush count — reachable only when the pool overflowed the 8 M edge budget while the GC
   lease had been acquired a handful of times (`acquireOrRenewLease` bumps `seq` on every acquire,
   `:4548`, `:4602`), i.e. in practice only under `setRebuildEdgeBudgetForTest`. The outcome is loud and
   self-healing anyway: `putDeterministicArtifact` refuses divergent bytes with `CORRUPTED_DATA`
   (`Gc/CasBlobInDegree.cpp:341-350`) and the next round's `seq` no longer collides. Structural
   closure is one line — derive the rebuild's attempt from `state.lease.seq` (e.g.
   `lease.seq * K + flush`), or seal/flush under `lease.seq` and keep the flush index in the run `seq`.

P3 — no silent corruption, no data loss; a bounded, eventually-pruned residue plus a false comment and
a missing audit row.

## `cas-gc-dryrun` reports `preview_deletes=0` for the damaged states a round fails closed on (2031-triage CAS-095) {#gc-dryrun-silent-on-damaged-state}

`Gc::previewDeletes` returns an empty vector when `gc/state` is absent
(`Gc/CasGc.cpp:4411-4413`), and — the sharper case the finding did not name — also when `gc/state` is
present and names an adopted seal that is GONE: `readFoldSeal` returns `nullopt`, `runs_by_shard`
stays empty, and every shard yields nothing (`:4424-4426`). A regular round treats that exact
condition as `CORRUPTED_DATA` ("adopted fold seal (generation {}, attempt {}) is missing",
`:3835-3838`, `listRefPrefix`), and `rebuildBaseline` refuses on it outright (`:3923-3931`). The
dry-run prints `preview_deletes=0` (`programs/disks/CommandCaGcDryRun.cpp:45`), indistinguishable from
a healthy pool with nothing to reclaim — in the one situation the tool exists for.

Two claims in the finding do NOT hold at HEAD. An UNREADABLE `gc/state` is not silent:
`decodeGcState` at `:4414` is outside any `try`, so it throws and the disks client surfaces it. And
the checksum verification (`reader.verifyAgainst(run.checksum)`, `:4478`) is likewise a loud
`CORRUPTED_DATA`, not silence — but the finding's consequence is right in kind: because the command
prints only after the whole vector returns, one bad run object discards every other shard's preview
instead of reporting per-shard. Related, prose only: the shipped description "Preview the next GC
round's deletes" (`CommandCaGcDryRun.cpp:23`) promises more than the API doc delivers — the preview
reads the durable sealed generation without folding new owner events and is explicitly a
quiescence-only subset (`Gc/CasGc.h:453-457`), which is the same gap `{#gc-followups}` `[F3]` tracks
from the measurement side.

Owed, cheapest first: (a) make the three no-baseline outcomes distinguishable in the output
(`state=absent` / `adopted_seal=missing` / `baseline=empty` beside `preview_deletes=0`), with the
missing-seal case an error exit since every other GC entry point fails closed on it; (b) emit
per-shard results and report a failed run as a per-shard error rather than losing the whole preview;
(c) align the CLI description with the `previewDeletes` contract. P3 — read-only diagnostic tool; its
output never authorizes a delete (`Gc/CasGc.h:440-441`).

## The ref-walk plan has two drop counters no production producer can ever raise (2031-triage CAS-096) {#refplan-dead-drop-counters}

P3, dead code plus one stale prose claim — no behaviour is wrong.

`RefPlan` counts five kinds of dropped adapter input (`Gc/CasGc.h:311-316`). Three are read: the
ordinary round emits `walk_plan_dropped_parent_rows` / `_listed_lives` / `_tails` as phase metrics
(`Gc/CasGc.cpp:660-662`). The other two are unreachable in production: `RefScanSummary::holds` and
`RefScanSummary::checkpoint_observations` (`Gc/CasGc.h:215-216`) are populated by nothing outside
`src/Disks/tests/gtest_cas_ref_catalog.cpp:1438-1453`, so `dropped_holds` (`Gc/CasGc.cpp:281`) and
`dropped_checkpoints` (`:292`) are always zero and `droppedHolds` / `droppedCheckpoints`
(`Gc/CasGc.h:298-299`) have no caller at all. A durable hold reaches the walk inside a parent row's
`RefLifeFoldState::coverage.hold`, so when its row is dropped the hold rides out on
`dropped_parent_rows`, never on `dropped_holds` — the counter that looks like the hold-loss signal is
the one that can never fire. Owed: delete the two adapters and their counters, or give them a
producer; and while there, put the dropped-row counters on the REBUILD path's own row, which today
carries eleven fields but none of them (`Gc/CasGc.h:94-118`,
`src/Interpreters/InterpreterSystemQuery.cpp:2385-2424`) — the reporting half is the debt already
owed by {#gc-followups} `[gc-rebuild follow-ups]`.

Same round, prose only: `CASGCUnmatchedAdoptedParentLives`'s description still says each occurrence
"is logged with its exact physical life id" (`src/Common/ProfileEvents.cpp:884`), but `4d40d453347`
deliberately removed that warning (it fired once per healthy completed removal and turned
`05023_cas_dropns_leaked_namespace` red). The counter is now the whole signal; the description must
say so.

## The round report's delete counters are tallied from the budget-capped outcome logs, and the fold events carry the previous round number (2031-triage CAS-101) {#gc-outcome-budget-skews-round-report-counters}

P3, observability only — two refinements to `BACKLOG.md`{#gc-round-budgets-not-backpressure} item D
("Audit loss"), which says the only casualty of `gc_round_outcome_entry_budget` is the `GcOutcomes`
audit row. It is also the operator-facing round row:

1. `report.deleted` / `absent` / `replaced` / `spared` are tallied by replaying the FINAL durable
   outcome logs (`Gc/CasGc.cpp:989-998`), and entries only enter those logs while the budget holds
   (`:852-856` for deletes, `:897-902` for spares; default 5000,
   `ContentAddressedSettings.cpp:83`). So on a round whose cohort exceeds the cap,
   `system.content_addressed_garbage_collection_log.objects_deleted` (fed by
   `Gc/CasGcScheduler.cpp:215-218`) undercounts deletes that really executed — precisely on the
   busiest rounds. The exact counter for the same work is `entries_redeleted`
   (`report.redeleted`, incremented unconditionally at `Gc/CasGc.cpp:857`, mirrored by
   `ProfileEvents::CASGCRetiredRedeleted`), so `objects_deleted + objects_absent + objects_replaced`
   silently stops equalling `entries_redeleted` past the cap with nothing saying why. Owed: either
   tally the report from the in-memory decisions rather than from the capped log, or document the
   skew at both the tally site and the column, and point readers at `entries_redeleted`.
2. `GcFoldBegin` and `GcFoldEnd` stamp `e.round = state.round` (`Gc/CasGc.cpp:719`, `:756`) — the
   round already committed BEFORE this one — while every other event of the same folding round stamps
   `new_round` (`:594`, `:607`, `:839`, `:889`, `:927`, `:949`). The defer path's use of
   `state.round` IS deliberate and documented (`:676-685`); these two are not, and `GcFoldEnd`
   additionally pairs that stale round with the POST-fold generation (`state.snap_generation` is
   mutated in-memory by `fold`, see `:764`). Effect: in
   `system.content_addressed_log`, a round's fold rows sit one round below its own delete rows. Owed:
   stamp `new_round` on both fold events (a one-line change) or say at the site why they differ.

NOT a defect, checked while triaging: `Phase` rows carrying `round = 0` is by design — the phase sink
copies the `Start` record (`Gc/CasGcScheduler.cpp:167`) and rows are correlated by `round_id`, which
`GcRoundLogRecord::round_id` (`CasGcScheduler.h:57-63`) documents as "DELIBERATELY NOT `round`"
because the round number does not exist yet on `Start` and never exists on a `NotALeader` round. Only
the column comment is imprecise: `ContentAddressedGarbageCollectionLog.cpp:40` says "0 on Start" and
should say "0 on Start and Phase rows; join on `round_id`". Likewise the constant phase metrics
(`walk_plan_builds=1`, `fold_seal_reads=2`, `Gc/CasGc.cpp:658`, `:670`) are recorded facts about how
many times the round reads one key, explained at `:667-670`, not measurements that broke.
