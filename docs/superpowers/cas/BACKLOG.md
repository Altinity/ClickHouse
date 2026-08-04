---
description: 'Consolidated live backlog of all still-pending CAS MergeTree work items. Single source of truth for what is left; issue IDs preserved (never renumbered). Groomed 2026-07-13, re-groomed 2026-08-04.'
sidebar_label: 'CAS Backlog (live)'
sidebar_position: 9
slug: /superpowers/cas/backlog
title: 'CAS MergeTree — Live Backlog (pending issues)'
doc_type: 'guide'
---

# CAS MergeTree — Live Backlog {#cas-backlog}

Live backlog: only open work. History and removed entries live in git; verification record in
`consolidation-2026-08/`.

## 1. Ref protocol — rev.6 lease-boundary exclusivity (highest-priority open design) {#ref-protocol}

- **[Late Predecessor PUT] cross-epoch late-materialization correctness limitation** — HARD — The hazard rev.6 closes: a fenced predecessor's in-flight PUT can materialize below successor snapshot coverage (a missed `−1`/`+1` = data-loss class). Phase-1 documents it; the fix LANDED as the v9 in-band `EpochSeal` (INV-2, Stage A). `CasRefLatePredecessorObserved` (B4) is deleted from the tree (a historical comment in `gtest_cas_ref_writer.cpp` remains); end-to-end LIST-liar fault injection = Stage A T13.
- **[MOUNT-CLAIM-EPOCH-REGRESSION] should `claimMount` permit epoch regression?** — QUESTION (surfaced by Stage A T12, 2026-07-29) — `claimMount` (`CasServerRoot.cpp` ~:395) reclaims a same-uuid body that is gc_fenced / clean-marked / proven-dead WITHOUT comparing epochs, so a fenced twin holding a HIGHER allocated epoch is legally reclaimable while the fresh writer proceeds with a LOWER `writer_epoch` — an epoch regression at the mount claim. Intersects the same-uuid recreation epoch-counter reset (quiesce = primary defence). Decide: must the claim gate require fresh `writer_epoch` above the reclaimed body's epoch (new `MountClaimResult` field), or is regression benign under the seal grammar? Sharp edge to verify: `prev_epoch_seal` is required iff `writer_epoch > life_epoch`, so a regressed writer may skip the seal obligation — confirm that path cannot readmit a Late-Predecessor window. T12 deliberately did NOT add a `chassert` here (it would abort a path the claim logic permits); surviving guards: unclean-reclaim classification, exhaustive `-Wswitch`, operator log line.
- **[refsnaplog Phase 2] measured ref-log/snapshot optimizations** — DESIRABLE (measurements-gated) — inline zero-byte log keys; GC-side fallback compaction for never-mounted tables; indexed/chunked multi-object snapshots; lazy snapshot blocks + byte-bounded row cache; per-round ref index; streamed snapshot construction; adaptive thresholds; decoded-body reuse; chunked namespace removal. Plus a **cross-epoch fault-injection integration test** reproducing the late-predecessor counterexample.
- **[timeout-retry RFC residuals] bounded lease-aware S3 timeout/retry controller** — PARTIAL — `CasRequestController` (single-attempt conditional writes, budget, fence-gating, exact-key resolution) landed for the ref lane. RFC `specs/2026-07-12-cas-s3-timeout-retry-control-rfc.md` residuals still open: (a) AWS SDK region-redirect retry can bypass `ShouldRetry` when a client is `aws-global` (CAS disks are not aws-global today — add a startup guard/probe if that changes); (b) `promoteStaged`'s `copyObjectConditional` (server-side conditional copy) is a separate conditional-write mechanism NOT bounded by the single-attempt work — verify its retry semantics before relying on write-once promote; (c) bounded read/HEAD/LIST retries + startup validation for the non-ref plain-object paths (`casPutObject`/`casRemoveObject` still use the disk's default retry policy).

## 2. GC scalability & byte cost {#gc-scalability}

- **[gc-frontier-one-list] Cheap change discovery: one LIST instead of per-namespace frontier GETs, plus a parallel walk** — DESIRABLE — USER-DIRECTED plan, 2026-08-03. `2026-08-03-list-trust-verdict.md` is the settled-facts reference; read it before re-arguing any of this.

  Today every `Live`/`Removing` namespace costs the round at least one exact `GET cursor+1` (the frontier probe, the only thing that sets `frontier_proven`), even when `tail == cursor` says nothing changed, and the ref walk over `walk_targets` is a plain sequential `for` on the round thread (`Gc/CasGc.cpp:2134`) — so a quiet 10k-table pool pays ~10k serial GETs per round for discovery alone.

  Two independent levers:
  1. **Trusted-LIST frontier mode (per-backend, passported).** On a store whose LIST is certified compliant (lists started after a completed PUT include it), the round's single frozen LIST tail at `tail == cursor` IS the frontier proof, so the probe GET for quiet namespaces can be skipped. Scope limits that stay unconditional on every backend: the MIDDLE stays arithmetic (predecessor-omission below a correct tail is legal S3 behavior); the probe survives for HELD namespaces and for namespaces with no listed logs at all; `tail < cursor` still feeds the store-quality detector. Certification = the `Cas::Probe` LIST-consistency passport (`[LIST consistency on real S3]`, §6). RustFS today FAILS the passport (proven-by-measurement omission, `2026-07-26-list-incompleteness-proof/`), so conservative (probe-always) stays the default and the mode is opt-in per backend, same shape as `[ckpt-read-policy]`.
  2. **Parallel exact-key walk (backend-independent, no trust change).** Fan the per-namespace probes and the `cursor+1..tail` record GETs over a bounded pool (reuse/extend `meta_pool`); per-namespace work is independent by construction. Wall time for N quiet namespaces drops N× serial → N/K; worth it under lever 1 too, for the hot namespaces that still fold records.

  MEASUREMENT: per-phase timing rows exist (`d412f85f749`) — record the discovery phase's GET count and wall before/after each lever.

- **[gc-snapshot-log-structured-runs] hot-pool snapshot rewrite is O(edges) per pass** — DESIRABLE — The single dominant remaining byte cost: a HOT pool rewrites the full snapshot run O(edges) per pass. Build O(delta)-write log-structured runs plus periodic compaction on the landed T2/T0 primitives (streaming reader, `seek`, `getStream`, ranged `get`, seal-ref resolution). Streaming reads and reference-parent runs (T2/T0) are already DONE; this is the remaining lever.
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
- **[clamp liveness] scoped suppression under long persistent clamps** — DESIRABLE→HARD (2026-07-18: concrete reproducer) — Fail-closed clamp+suppression is correct and self-heals (false-404 attribution), but suppression-vs-liveness under long clamps is unaddressed (scoped suppression later). Clamp observability (clamped key/shard event) is DONE. The S38 sub-finding of 2026-07-18 — a poison late log clamps its own key and thereby starves `reportLateLogsIfAny` indefinitely (40 healthy Success rounds, sweep pass suppressed in every one, `RefLateLogDetected` never fires, `2026-07-18-s38-late-log-clamp-starvation.md`) — is MOOT as of Stage A task 6 (`d74c726ef9e`): the LIST-based late-ref-log detector it starved no longer exists, and a late log is now fenced by an in-band `EpochSeal` rather than reported after the fact (S38 asserts that fence directly). The general item stands on its own reproducers; the starvation shape is kept here only as the record of why a report-after-the-fact detector was the wrong shape.
- **[gc-rebuild follow-ups]** — MINOR — Dedicated gc-round-log row for `rebuildBaseline` (currently only `LOG_INFO` + a `gc_rebuild` event); the "unowned-alive manifest edge over-protect" documented leak (bounded, fsck-visible, cleared by a future rebuild); soak validation (`mc rm gc/state` mid-soak → guard fires `CORRUPTED_DATA` → `SYSTEM … GC REBUILD` recovers to dangling=0).
- **[fsck oracle gaps]** — MINOR — fsck under-reports orphan manifest bodies for ref-less namespaces (enumerate `cas/manifests/` too, not just `cas/refs/`+`roots/`); fsck Orphan-class test gap.
- **[REBUILD R4 residual — manifest-less blobs unreclaimable]** — TRACKED, by design until R4 — Since Task 11 a rebuild condemns nothing (spec §7 — the zero-edge LIST/HEAD sweep was the r5-finding-4 data-loss vector), so a blob whose manifest no longer exists anywhere in the pool has no row in the rebuilt baseline and the incremental pipeline can never reach it. Such blobs are RETAINED and show as fsck `unaccounted` that does not drain after a disaster rebuild. This is the NAMED staging-contract residual of register R4 (the build/upload registry, which is what can enumerate in-flight uploads safely). NOT a bug and explicitly NOT to be closed with a substitute reclamation: any rule that reclaims from an enumeration reintroduces the same vector. Closes when R4 lands.
- **[repointRef non-resolving-key audit gap]** — MINOR — `CachedPartFolderAccess::repointRef` (`CachedPartFolderAccess.cpp:283`) increments `CasRefRepoint` and logs "Repointed committed ref…" unconditionally after its `if (resolved)` byte-equal check, even when `resolve(key, ForceFresh)` returns `nullopt` — i.e. it would count/log a repoint for a key with no existing committed ref. Unreachable today (every caller — Task 4's standalone writes, Task 8's removal-mark resolution — only calls `repointRef` on an already-resolving key); a defensive `throw LOGICAL_ERROR` on `!resolved` would make the precondition explicit and the counter/log trustworthy rather than merely-currently-true. (Found during all-tree Tasks 7/8 integration review.)
- **[ProvenanceOp operability gap]** — MINOR — Both the Task 4 committed-ref standalone write and the Task 8 removal-mark repoint call `repointRef(..., Cas::ProvenanceOp::Other)` — no distinct op kind for a removal-repoint vs a write-repoint in `system.content_addressed_log`. Spec doesn't require one; would help an operator distinguish "this repoint dropped files" from "this repoint added/changed files" in the audit trail without decoding the entry diff. Product-owner call, not decided during Task 8 integration. (Found during all-tree Task 8 integration review.)
- **[codex-11] namespace drop misses an unregistered build → ownerless Live namespace** — LOW — `Pool::beginPartWrite`'s allocate/register window (`CasPool.cpp:772-777`) is real: a build can allocate before the drop sweep (which only snapshots `inflight_builds`) runs, then legitimately pass the birth-time marker gate afterwards, reviving a Live-but-ownerless EMPTY ref-table — a small, non-self-healing metadata leak (GC never sweeps Live namespaces). Confirmed narrow, LOW severity (2026-07-17 codex-review triage, finding №11); the reviewer's atomic-registration fix would NOT close it (the same TOCTOU recurs between the `cancelled` check and the append for already-registered builds). Fix direction: a GC backstop that reclaims empty ownerless Live namespaces, or a namespace generation folded into the birth-time marker gate.
- **[RECOVERED-INDEGREE-ATTRIBUTION] move the "delete_pending recovered in-degree" invariant check to the writer** — DESIRABLE — The GC-side `LOG_WARNING` at `CasBlobInDegree.cpp:418`/`CasGc.cpp:485` fires as a false alarm (56x/run on tiny system-log blobs): root-caused to a dedup-adopt-vs-condemn TOCTOU where `observeAndAdmit` (`CasPartWriteTxn.cpp:354-421`) adopts a token just before GC condemns it, and GC correctly spares it (no data loss). Fix: downgrade the GC log to a `ProfileEvent` + `LOG_DEBUG`, and add a real detector at the writer's edge-commit (typed `BlobAdoptRacedCondemn` event) so the false alarm goes away without losing the one genuine bug class it masks. RCA in `project_pr2073_ci_triage_2026_07_23`.
- **[CONDEMN-GRACE-WINDOW] cool-down before condemning a just-zeroed blob** — DESIRABLE — The driver of the RECOVERED-INDEGREE noise: tiny system-log blobs whose in-degree hits 0 and is re-referenced by dedup almost immediately. A short grace/cool-down before condemning a blob whose in-degree just transitioned to 0 would remove the churn, but this changes condemn timing and touches GC invariants (retention vs. reclaim latency, ack-floor interaction) — needs TLA-level reasoning and is subject to the protocol-step-change veto, not a "cheap" tweak. Measure first: count re-condemn/HEAD/spare cycles on the 28 hot hashes from run 30019911967 to size the benefit.
- **[REBUILD-SEAL-POINT-READ] point-read closure for REBUILD seal discovery** — HARD — `rebuildBaseline` finds the pool's newest fold seal via a bounded probe-and-step-down over `gc/gen/<G>/` listings (probe-A-style detection, not proof) because there is no dense point-readable attempt id to walk. Two residuals: (1) a store that lies about one generation's own prefix can still hide it; (2) the "virgin pool, no holds" verdict can be silently wrong on a PRUNED pool (`CasGcRebuildVirginByEnumeration`, visible but not prevented). Fix: a write-once `gc/gen/<G>/sealed` alias minted at adoption makes discovery a dense exact-`GET` walk, closing residual (1); residual (2) needs something that survives pruning (an unpruned marker location, or a pool-level floor pointer) — a small protocol/format amendment, deferred to the spec-amendment decision. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-8-report.md`.
- **[STAGE-B-7B-SEQUENCING] `UniversePolicy::kDefault` must not flip before Stage B's incarnation-keyed cursors land** — {#stage-b-7b-sequencing} — HARD CONSTRAINT, NOT A TASK — Stage B's Task 7b (`kDefault = StageA_Suppressed` → frontier-consulting) must not happen first: Task 9's destructive-gate universe union means a namespace removed and recreated inside one writer epoch can restart its ref-txn ids at or below the retained cursor, so the walk can miss the recreated edges entirely — live blobs would look unreferenced. Contained today because Stage A destroys nothing; the containment evaporates the instant 7b flips. Structural closure: cursors keyed by `(namespace, incarnation)` rather than by name. Full reasoning: `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-9-report.md` §12.
- **[SUPPRESSED-HANDOFF-CONSUMPTION] a suppressed round consumes the hand-off reclaim instead of deferring it** — {#suppressed-handoff-consumption} — MINOR (bounded leak, fsck-visible) — The post-CAS hand-off (`handoff_reclaim` phase) reclaims a generation the wholesale prune skipped; under Stage A's blanket suppression this DROPS rather than defers, and nothing revisits it — left to `fsck`, same class as a pre-existing crash-window leak, just more frequent. Stops being systematic the moment `{#stage-b-7b-sequencing}` flips `kDefault`. Pinned by `CasGcFrontierGate.TheHandOffReclaimIsInertUnderSuppression`.
- **[RECOVER-REF-TABLE-LIST-RESIDUAL] the free-function recovery equation is still LIST-driven — a named 7b precondition** — {#recover-ref-table-list-residual} — 7B PRECONDITION — `recoverRefTableDetailed`/`recoverRefTable` (`Pool/CasRefProtocol.h`) still recover by full LIST + replay while the production mount path and fsck's `checkRefStream` walk arithmetically; a hidden middle record fails closed but blind (`clean()` true, `reachable=0`), and a hidden tail record makes fsck report clean over a ref set missing an acked publish. Consumers: fsck's per-namespace replay, the orphan-manifest sweep's deletion premise, `Gc::rebuildBaseline`, and fsck's dangle revalidation. Must resolve, or be proven non-exploitable through the named gates, BEFORE `{#stage-b-7b-sequencing}` flips `kDefault`.


- **[FSCK-SCALE-TIMEOUT] `ca-fsck` cannot complete a large pool within its own deadline** — {#fsck-scale-timeout} — MEASURED — At ~29-31 GiB the audit times out (`FSCK_EXIT=159`) and returns nothing at all — "fsck clean" is unmeasurable exactly where an operator wants it most; raising the harness budget 180→600s did not help (the cost is the pool, not the budget). Direction: bounded/streamed partial verdicts (per-namespace pagination with a resumable cursor) and deadline-aware partial reporting via the existing `partial`/`partial_reason` fields. Structural consequence: a phase-3 soak's fsck-clean gate stays reported UNARMED under Stage A at any pool-growing workload; complete audits at auditable scale (05020 + scenario end-checkpoints) remain the real evidence. Related: `{#soak-fsck-checkpoint-budget}`, `{#gc-scalability}`.

- **[CA-LOG-TABLES-RESTART-COST] the CA instrumentation's Outdated churn failed the restart health gate** — {#ca-log-tables-restart-cost} — NEW (gc-audit 2026-07-29) — the 6/40 soak's post-kill restart took 178.9 s against a 180 s gate: 40.0 s CAS mount-lease token-stability wait + 138.1 s reloading SYSTEM-LOG tables' Outdated parts (`system.content_addressed_log` alone: 299 Outdated parts; the USER table loaded 1 part in 15 ms). The observability that makes a soak a specimen is what failed its checkpoint. Directions: TTL/partitioning for the CA log tables, bounded event-log part churn, lazy system-log load. Related: the merge-churn datum (generations 74k+/35 min).
- **[CA-GTEST-TMP-SCRATCH-LEAK] every full CA gate leaves thousands of scratch dirs in `/tmp`** — {#ca-gtest-tmp-scratch-leak} — MINOR (TEST/INFRA) — CA gtest fixtures create scratch pools in `/tmp` and never clean them on teardown (`cas_unit_*` + siblings, ~50k dirs accumulated, once drove tmpfs to inode exhaustion mid-build). A separate, still-unexplained debris family (`k/ p/ pool/` cwd droppings) was confirmed to be actively RECREATED every gate run rather than pre-existing residue, so some test still writes CWD-relative on every run — the obvious suspects (`cas_test_helpers.h`'s `makeLocalObjectStorageForTest`, the `"pool"` key-prefix arguments in `gtest_cas_forget.cpp`/`gtest_cas_operation_gate.cpp`/`gtest_cas_mount.cpp`) are exonerated. Fix: teardown-time removal in the shared CA test fixture, or scratch under the build dir instead of `/tmp`; bisect the cwd-writer by running suite groups with a clean CWD.
- **[ORPHANED-ADJUDICATION-COMMENT] `CasRefLedger.cpp:108-120` documents an adjudication its neighbouring code does not perform** — {#orphaned-adjudication-comment} — found 2026-07-30 by the Task-1b implementer while attempting (and correctly reverting) an exposure of `chainLinkFor`: the comment describes a `mine | successor's seal | foreign` adjudication with a narrow `catch`, which is NOT what the function beside it does. SMALL but real — a comment that misdescribes its neighbour is worse than no comment, and this region is exactly where the next reader will look when INV-2's chain-link grammar is next touched. Take it with Task 1c's sweep or the restatement pass, whichever reaches the file first; re-derive what the comment SHOULD say from the code rather than deleting it blind. Related: `chainLinkFor` stays in an anonymous namespace, so INV-2's grammar cannot be swept in isolation — asserted through `prepareRefChunk`'s validator instead (accepted disposition, Task 1b).
- **[DEAD-INSTALL-PROBE-AND-STALE-REGION-COUNT] the post-durable install seam has a stale region-count comment** — {#dead-install-probe-and-stale-region-count} — MINOR — The dead-test-hook half is fixed (`gtest_cas_ref_ckpt.cpp`'s carve-time fence now sets the probe). Still open: `CasRefLedger.cpp:1903` says "Post-durable install region 2 of 3" although the restatement deleted the third region, and the fence's own comment now says "BOTH" while this comment still says "2 of 3" — a self-contradiction. Placed as plan Task 4 Steps 0a/0b.
- **[LANE-TERMINAL-REPORTED-AS-RETRYABLE] one arm sets `Faulted` and hands survivors the retry-later class** — {#lane-terminal-reported-as-retryable} — MINOR (one-line fix) — `commitRefChunk`'s "lane not `Ready` at new-id allocation" arm sets `RefLaneState::Faulted` but completes survivors with the retry-later exception class, contradicting the stated contract (`Faulted` should map to `CORRUPTED_DATA`). Self-limiting (one spurious retry, not a loop), but a contract worth stating is worth not contradicting in one arm. Placed as plan Task 4 Step 0a.
- **[LANE-WITNESS-NAMES-MORE-THAN-IT-PROVES] a lane-battery witness proves less than its name, and one adoption arm has no witness at all** — {#lane-witness-names-more-than-it-proves} — MINOR — `saw_retry_created` witnesses that a retry created durability, not that the adoption install happened, but `CaRefLaneCore_RESULTS.md` calls it "retry-created adoption" — overstated. No witness asserts the `Wedged → Ready` durable-adoption arm at all. Fix: correct the RESULTS wording and add a witness on the adoption install itself. Placed as plan Task 10e; does not affect the blocker-dissolved verdict.
- **[PART-WRITE-RELEASE-SEAM] the `PartWriteTxn`/`PreparedPartWrite`/receiver-guard ownership seam needs its own contract spec** — HARD — USER-DIRECTED extraction, 2026-07-29. The relink redesign's review rounds kept grinding on one seam: three layers each with their own abort/retry, an overloaded `isTerminal`, nine scattered proven-no-send exits erased into a generic `NETWORK_ERROR`, false ERROR/WARNING log lines on settled-late releases, and no exactly-once emission contract for unproven releases. Extracted into `docs/superpowers/specs/2026-07-29-cas-part-write-release-seam.md` as relink-independent prerequisite plumbing (single-`attempted`-bit proof channel, destructor-owned last-word emission, severity ladder, marker-sync fix); lands before relink implementation.

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
- **[GC-DEFER-DECISION-LIST-COST] the round's whether-to-fold decision costs a full pool LIST — 79% of all GC time** — {#gc-defer-decision-list-cost} — TRACK-B HEADLINE — With rounds now bounded, the leader is still busy 90.5% of wall, and 79.11% of it is `defer_decision`: a full ~177k-key LIST every round, sometimes concluding `changed_shards:0` after 127s (the idle-round control case costs 5 orders of magnitude less). The frozen-tail design requires a LIST to discover tails, so any fix is design work: candidates are a scoped per-namespace LIST with start-after markers, tail discovery from the previous round's coverage plus delta probing, or LIST-page caching keyed by namespace tail. PROTOCOL-ADJACENT — user consult before any change to what the round reads.
- **[GC-FULL-TIME-ACCOUNTING] every millisecond of a GC round must be attributed** — {#round-duration-alarm} — TRACK-B ITEM — Two measured blindnesses fixed the visibility gap: a never-completing round used to be invisible (no Finish row), and completed rounds showed minutes living outside every timed phase span. Timer coverage is now measured 99.986% complete (0.9s unaccounted out of 6,443.4s), with the three remaining un-timed spans identified and all sub-20ms. Remaining work: name the `orphan_sweep` epilogue phase, add an `unaccounted_ms` self-check column to the Finish row, and add a periodic in-round progress log line past a sane elapsed bound.

- **[POOL-REFUSAL-NODE-FATAL] a pool bootstrap refusal takes the whole node down** — {#pool-refusal-node-fatal} — DESIGN QUESTION (2026-07-29, surfaced by the W3 RCA; pre-existing bootstrap behaviour, NOT Stage A) — the residual-data guard (`CasPool.cpp` ~:439, Code 668 `missing _pool_meta over a non-empty pool prefix`) raises during metadata loading and propagates out, so the SERVER EXITS (container exit 156) instead of starting with that one disk marked unusable. Refusing the pool is right (fail-close); taking the node down for one residual CA prefix is the question — a node may serve many disks/tables that are healthy. Direction: bootstrap-refusal -> disk marked broken/read-refused + loud diagnostics + the node UP, consistent with the disk-lifecycle redesign goals (UNMOUNT ejects, FSCK not dormant-only); the refusal message already names the operator verbs (recreate or restore `_pool_meta`). Evidence: S43's W3 answer (refusal + causation control), `2026-07-28-stage-a-RESULTS.md` row 14.


- **[SEAL-DECODE-REMAINING-FIELDS] the rest of the silently-defaulting-field family in the fold-seal codec** — {#seal-decode-remaining-fields} — SMALL FOLLOW-UP (2026-07-29, T16 concern 2, deliberately left out to keep the F1 diff reviewable) — `btr` missing `key`/`ck` and `cnd` missing `shard` default silently exactly the way `cls` did before T16's fix; same treatment owed (required-field refusal, CORRUPTED_DATA). One small task, same test file, after T16 merges.

- **[cas-format-version-floor] `checkCompatibility` never rejects a version below a type's own birth generation** — {#cas-format-version-floor} — DESIRABLE — `checkCompatibility` (`Formats/CasFormat.cpp`) throws `UNKNOWN_FORMAT_VERSION` above `G_BUILD` but accepts anything below it, including a version under the type's own birth generation (e.g. a header claiming generation 1 for a type born at generation 4 decodes as legal); `decodeRefCatalog` also discards the parsed header once the check passes, so nothing downstream can recover the version for logging. Not required by the empty-universe GC gate fix that surfaced it — closing this floor would only shrink an already-accepted residual, not remove it. Fix: a per-type birth-generation floor enforced centrally in `checkCompatibility`, refusing a version below `changePoints(id).front().generation` as `CORRUPTED_DATA`; blast radius is the shared format layer used by every CAS object type, so needs its own failing-first coverage and a fixture/artifact audit.

## 4. Read / write path {#read-write}

- **[ckpt-read-policy] Modular `_ckpt` first-attempt view: conservative / cached / prefetch** — DESIRABLE — USER-DIRECTED design shape, 2026-08-03: `_ckpt` handling must be modular/replaceable. Protocol-adjacent (touches the commit path); ships as an explicit reviewed decision with before/after numbers, per the `HEAD`-before-`PUT` protocol-step veto.

  Cost being addressed: every committed ref-log chunk pays `GET _ckpt` + token-CAS serially after the log `PUT`, so a lone `INSERT` pays +4 serial RTTs. A pluggable policy chooses only where `publishCkpt`'s first attempt gets its `{body, token}` view — the invariant core (retry-after-conflict always does a whole-body exact re-read, `lifeEpochWouldDecrease` re-checked after any re-read, durability order `log PUT → _ckpt CAS → ack`) is shared and policy-independent.

  Policies: (1) **conservative** = today, fresh GET per publish; (2) **cached** = seed with one GET on first touch, then serve from the writer's own last winning CAS and go straight to PUT-if-match (expected to almost always hit, since lease exclusivity excludes cross-process writers — a miss is a signal, not noise); (3) **prefetch** = one paginated LIST at mount seeds all `_ckpt` views, then memory-only + PUT-if-match (LIST is a pure hint here, correctness still rides the conditional write). Mandatory cache-invalidation edges for (2)/(3): fence-generation change, wedge, remount supersession, `catalog_life_invalidated`. Always-exact-read, out of the policy seam: recovery's `_ckpt` sample and GC-fold's frontier GET.

  Effect: +4 → +2 RTTs per lone insert. MEASUREMENT PRECONDITION: the stage-1 1.59x figure predates `_ckpt` (measured before it landed) — re-run the wide-insert baseline on current HEAD before benching policies against it.
- **[write-path stage 1] parallel intra-part blob upload — LANDED (2026-07-24)** — Fanned out a part's blob PUTs/dedup-HEADs (`CasBlobUploadPool`/`fanOutBlobUploads`): CA wide-insert wall 58.41s → 30.26s, CA-vs-plain 3.0x → 1.59x. Residual gap = the serial cross-part commit (stage 2, `{#stage2-concurrent-commitpart-postponed}`, POSTPONED by user decision) plus the CAS-only dedup HEAD/GET traffic (`[B121 / B202 / one-GET-open]` below).
- **[TXN-ONE-PIPELINE] complete the "staging ops never defer" invariant** — HARD (small, structural) — `DiskObjectStorageTransaction`'s two dispatch pipelines (eager staging ops vs. deferred-to-commit durable ops) caused the `01603` column-TTL abort ordering inversion; the correct invariant is per-state-domain, not a total order. Target shape approved by the user and superseding earlier staged-intents wording: an everything-immediate model with a single `dispatch` funnel (no CA subclass), a two-phase `IDiskTransaction::precommit()`/`commit()` contract (CA precommit = the entire publish; CA commit = durable-intent materialization only), `commit` implicitly running `precommit` when not called (with `CasImplicitPrecommitInCommit` observability), plus a de-patching pass removing accumulated eager-dispatch/read-your-writes workarounds from non-CA files (`docs/superpowers/cas/upstream-patch-inventory.md`). SPEC: `docs/superpowers/specs/2026-07-15-cas-txn-one-pipeline-design.md` — lands before codecs v3 and the source-layout refactoring.
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
- **[F1-prod] read-only same-pool shadow disk (`ca_ro`) breaks table load on restart** — GATE (prod) — MergeTree part discovery finds every part twice → `UNKNOWN_DISK` on restart with CA tables. Stand workaround shipped (standalone `clickhouse-disks -C` fsck-only config; propagated to the default stand); PRODUCT fix (part discovery skips `readonly` same-pool disks, or a `hidden`/`introspection_only` disk flag) still open; `10replicas`/`gc_shards2`/`awss3` server configs may still embed `ca_ro`.
- **[B165] server OOM at hour-4 soak (~49 GiB RSS)** — VERIFY — Not reproduced since the `putBlob` streaming fix; re-run a long soak to confirm resolved.
- **[B14] expedited / GDPR right-to-erasure delete** — DESIRABLE — Under GC lock, confirm no live ref, then delete bypassing the two-phase graduation delay; no layout change.
- **[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation.
- **[B26 / B135] [B66a] → §14** — local/emulated-backend items collected into §14 {#local-backend} (2026-07-23 grooming, user direction: local-backend stories live in ONE section).
- **[B66b] relink-into-detached (zero-byte `to_detached` fetch for same-pool parts)** — IN PROGRESS (2026-07-23) — folded into the publish-confirm fetch-handoff iteration (spec `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md`): relink already publishes under `tmp-fetch_<part>` and re-keys via `renameTempPartAndReplace`, so detached needs only lifting the `!to_detached` advertise gate (`DataPartsExchange.cpp:540-545`) + the detached temporary name + the same confirm step; collision semantics inherited from the byte path by construction. (RPL-4 perf cliff.)

## 8. Mount-lease / fence recovery {#mount-fence}

- **[P3.1 Task 6 / S13] live validation of fence-recovery** — TEST — TLA+ gate PASSED and the correctness paths landed (self-remount on GC fence-out is DONE); the gtest sweep + S13 3×-green live gate remain. **Task 5** (decouple renewal from the retired-view sync beat) is likely **MOOT** — freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`; confirm and close.
- **[A7-residual] gc_scheduler lifetime vs manual rounds** — VERIFY — Believed addressed by `89845c2a544` (shutdown serializes gc_scheduler teardown with health reads; wedged-lane count pinned) on top of the stabilization A7 fix. Confirm no residual: (a) a manual round on a raw pointer captured outside the lock, (b) lazy creation resurrecting a scheduler after shutdown.
- **[STID-3982-3b48 part 2] mount-lease self-race Gate 3 re-run still owed** — {#stid-3982-3b48-part-2} — TEST — A third variant of the mount-lease renewal self-race (ambiguous client-side timeout on the renewal `PUT` misdiagnosed as a foreign-writer collision, SIGABRT under ASan) is fixed and landed 2026-07-24 (fence-not-rescue redesign, spec `specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md` rev.4, TLA+-gated, full `Cas*:CA*` green). Open: Gate 3, the live CAS-s3 stateless-lane validation that originally caught the crash, has not been re-run post-fix — rides the next CI push of `cas-gc-rebuild`.

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
- **[RPL-5 slice] `REPLACE PARTITION`/`ATTACH PARTITION ... FROM` queue-clone relink, untested on CA** — TEST — A `REPLACE_RANGE` log entry cloned to a second replica reduces to fetch (relink or byte) + drop, individually working, but no integration test proves the cloned fetch specifically relinks rather than byte-refetches (RPL-4 disables `to_detached` relink explicitly, so the branch taken isn't obvious a priori). Needs `test_cas_replicated_relink`'s 2-replica rustfs fixture extended with a `REPLACE PARTITION`/`ATTACH ... FROM` scenario plus a blob-count relink proof. Pulled into the publish-confirm fetch-handoff iteration's test package (spec `2026-07-23-cas-fetch-handoff-publish-confirm-design.md` §testing), which touches the same relink-eligibility branch.

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

- **[remote-data-paths-no-pushdown] `system.remote_data_paths` walks every disk — no `disk_name` pushdown** — {#remote-data-paths-no-pushdown} — DESIRABLE (upstream, needs consultation before editing generic code) — `StorageSystemRemoteDataPaths` ignores `WHERE disk_name = …` (TODO still in `StorageSystemRemoteDataPaths.cpp:153`), so a query against it walks every disk including a large shared CA-s3 pool's paginated ref-namespace LIST — this timed out `04286_content_addressed_remote_data_paths` at 600s (root cause, not a CAS regression; `NOT` the mount-lease path). Mitigated 2026-07-27 by tagging that one test `no-content-addressed-storage`; the `applyFilters` disk-name pushdown itself stays open as the real, general fix.

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

- **[source-layout-bisect-hazard] source-layout intermediate commits `592b9b8..9d714dd8` are not clean-buildable — a bisect hazard** — MINOR — A Phase-2 include sweep stranded 3 external-consumer include fixes outside the sweep commit's pathspec, so those intermediate commits reference moved CA headers at dead paths (per-step gtest only looked green because incremental builds saw uncommitted working-tree fixes). Accepted as-is (dev branch, not upstream, no-amend/no-rebase rule) — a bisect landing in that range fails to build the external consumers; document and route around it. Lesson for future reorg sweeps: pathspecs must include every sweep-touched file including external consumers, and verify the COMMITTED state builds, never trust incremental-build green for a move/sweep.

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

- **[move-part-to-ca-architecturally-unimplemented] `MOVE PART`/`PARTITION` onto a content-addressed disk is architecturally unimplemented** — HARD — `ALTER TABLE ... MOVE PART|PARTITION TO DISK|VOLUME <ca disk>` throws `ABORTED` ("promote: ref already names a different committed manifest") or `NOT_IMPLEMENTED` depending on which file the generic per-file copy reaches first. Root cause, two layers: (1) `PartPathParser`/`ContentAddressedMetadataStorage::route()` have no case for `MergeTreeData::MOVING_DIR_NAME`, so every moved part's files collide on the literal ref name `"moving"`; (2) deeper and not fixed by (1) alone — `MergeTreePartsMover::clonePart`'s generic per-file copy opens a fresh autocommit `DiskObjectStorageTransaction` per file with no shared transaction threaded through the whole part clone, so the second file's independent `promote()` collides with the first's. A real fix needs a CAS-aware `clonePart`/copy override (mirroring the existing `supportZeroCopyReplication()` special case in the same function) that stages every file into ONE `PartWriteTxn` and commits/promotes once. Blocks all of S36 (MOVE is its first leg) and S37's explicit-MOVE and TTL-MOVE-to-`cas` legs; unaffected: routing at INSERT time (`max_data_part_size_bytes`), merges writing a brand-new part, and off-CA moves (CA→local).

- **[ca-scratch-path-docker-entrypoint-permission] a CA disk's default scratch path can be root-locked by the docker entrypoint** — {#ca-scratch-path-docker-entrypoint} — DOC — If a sibling `local`-type disk declares an explicit `<path>` under the same `<data-path>/disks/` tree as a CA disk's default (undeclared) scratch path, the official docker image's entrypoint `chown`s only the leaf it was told about, leaving the shared `disks/` parent root-owned — the CA disk's later scratch-dir creation then throws `Permission denied` and the server exits before listening. Not a code bug (fail-closed is correct); a deployment nuance worth documenting: keep any sibling local disk's declared path outside the CA disk's `disks/<name>/` namespace. Already worked around in the ca-soak harness configs; open ask is to carry the warning into user-facing deployment docs.

- **[merge-progress-reset-mount-fence] merge "progress reset in loops" under sustained S3 fault = mount-fence loss + ABORTED-defeated backoff** — HARD — Root cause (not a merge-vs-insert retry gap — the CAS upload-retry stack is caller-agnostic): sustained faulting fails the mount-lease renewal PUT, the fence trips, and every write fails `stageManifest`'s `fence_ok()` gate instantly until self-remount recovers; fail-closed correct (no data loss, self-heals), but the replication scheduler tight-loops recomputing the same merge (239x in one repro). Three CA-side defects to fix, priority order: (1) OVER-FENCING — `SingleWriterSlot`'s renewal loop burns the whole writer incarnation on the first transient renewal-PUT exception instead of retrying while the lease deadline is still valid; fixing this alone kills the common transient-blip case. (2) `ABORTED` DEFEATS THE EXISTING MERGE BACKOFF — CAS throws `ABORTED`, which `ReplicatedMergeMutateTaskBase` treats as "not an error" and never records, so upstream's existing exponential backoff never engages; needs a retry-later class thrown outside the `ABORTED` exemption at the fence-lost boundary specifically (genuine merge cancellation must stay `ABORTED`-exempt). (3) OPACITY — the "retry budget exhausted" message fires when nothing was attempted, and failures log at Information only (invisible in `system.replication_queue`). Also unverified: fsck-to-fixpoint after a fence-loss recovery, to confirm "no orphans" (links to the S30 DANGLING-PRECOMMIT gap). Repro harness: `utils/ca-soak/docker-compose-s3faultproxy.yml`.

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

- **[r3-acked-lost-dataloss] acked-then-lost INSERT data loss on cross-request retry — FIXED** — Root cause: on CA, `renameParts` is a pure overlay re-key with no publish, so the Keeper block_id/part-znode multi commits durably BEFORE the CAS manifest — a genuine split-commit window. A part lost in that window left its dedup token behind, so a byte-identical client retry deduped against it and acked with zero rows written. Fixed by `77484196b0d`: closes every part disk-storage transaction in `MergeTreeData::Transaction::renameParts`, so the part is durable before the Keeper block_id registration. Gates all green post-fix (S40 10/10 acked=3796 lost=0; `dl_probe` LOST=0, was ~198/1314; the original R4 chaos recipe that lost 1118 rows now PHASE3 OK with zero deficit). R3 ship-readiness restored. Residual: a narrower hazard (block_id outliving a durably-committed part lost later) stays out of scope — verify-on-dedup is the candidate if it ever matters. Open thread: an upstream submission draft exists (`tmp/upstream_issue_dedup_durability.md`), pending a user decision on whether to send it.

- **[B208] CA startup mount-probe is fail-closed against a transient S3 outage — server aborts and stays down** — DESIGN QUESTION — A server started while the object store is unreachable dies during metadata load (mount startup capability probe times out, exit 243, no retry, stays down until an operator restarts it). Product question: bounded startup retry / degraded-start (mount later, serve non-CA tables meanwhile) vs. fail-closed correctness (a server that starts without its pool must not fake readiness). Not a gate for the durability fix (S40's contract only requires acked data to survive on the live replica). When fixed, add an informational recovery verdict to S40 so a regression here produces signal.

- **[s39-ci-scale-fault-window] S39's ci-scale fault window violates its own timing assumption** — MINOR (GREEN-DEBT, tracked return-item) — The ci param row's short-fault window doesn't complete inside a renewal period, so the no-fence assertion isn't sound at that scale (dev scale is correct and is what all prior green runs used; ci scale had never actually run). Fix: size the ci row off the same 30s TTL anchor as dev, then run S39 at ci scale to green.

- **[build-dir-rust-localize-drift] local build-dir config drift: rust `localize_rust_c_*` rules lost their reference-library args** — MINOR (GREEN-DEBT) — A past cmake reconfigure produced a `build.ninja` where every rust-contrib localize rule (chdig, polyglot, wasmtime, delta_kernel_ffi) carries zero reference-library args, so any future `ninja` touching a rust contrib fails in this build dir. Fix: full cmake re-configure of `build/`, and identify which configure produced the argless state to guard against recurrence. The nightly image build was unaffected (built from a binary that predates this).

- **[gc-checkpoint-timeout-tsan] ca-soak GC-checkpoint timeout formula assumes normal-speed GC throughput — blows the budget under TSan** — MINOR (GREEN-DEBT) — `soak/checker.py:fixpoint_timeout_s` assumes a normal-speed GC reclaim rate; under TSan's instrumentation overhead a genuinely-converging backlog (confirmed trending down, `dangling=0` throughout) can still blow the budget — a harness-timing artifact, not a correctness bug. Fix: add a sanitizer-aware multiplier to `fixpoint_timeout_s` (or an explicit CLI override); low priority since the gtest battery and other soak stages already validate TSan correctness.

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

## CAS disk lifecycle rev.8 round (FORGET-only) — residuals {#disk-lifecycle-rev8-closure}

Round: spec `docs/superpowers/specs/2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md` (rev.8,
FORGET-only); plan `docs/superpowers/plans/2026-07-22-cas-disk-lifecycle-rev7.md` (17 tasks); problem framing
`docs/superpowers/specs/2026-07-22-cas-disk-lifecycle-problem-and-constraints.md` (goals G1–G7). G1-G5
resolved this round (isolation fix, throw-not-abort, GC self-exit on Vanished/IdentityLost, generic-code
correctness, FSCK-on-running advisory).

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

- **[partb-review-findings] `eraseView`/`publishStaging` no-throw-after-commit residual** — {#partb-review-findings} — MINOR — The 2026-07-25 publish-confirm protocol review's two blockers and two majors are fixed (`8e6fe6ef0af`); the one residual window: `eraseView` still runs after the durable commit and can throw, and `ContentAddressedTransaction::publishStaging`'s `out_slot` is assigned only after `promoteBuild` returns — closing it means extending the no-throw-after-commit discipline one frame outward.

- **[fsck-large-pool-fixed] fsck large-pool reporting: three residuals after the 2026-07-26 fix** — {#fsck-large-pool-fixed} — MINOR — `corrupted_runs` visibility/fatality, inverted timeout budgets, and fabricated-clean-on-partial are fixed. Open: (a) `checker.py`/`run.py`/`plot.py` still print the old `M-F debris, B140` label for what the product now classifies as `AwaitingGc` (docstring cleanup only); (b) `FsckTimeout` still substitutes fabricated `{"dangling": 0, ...}` zeros on the remaining timeout path — landmine, not a live defect (every consumer guards on `not _detail_fsck_skipped`), fixing it needs auditing every downstream `f.get(...)`; (c) the GC-checkpoint entry-gate fsck still does not finish on a 5.5 GB pool within its 180s budget — options not yet chosen (scale the budget, use `--partial` as a lower bound, or make fsck cheaper).

- **[S42-ci-verdict] S42 memory-exhaustion card needs a clean re-run to certify** — {#s42-ci-verdict} — TEST — `ci`-scale S42 passed every safety signal (2,184 injected allocation faults fired, `CasRefApplyPoisoned=0`, both wedged ref lanes recovered, `CasGcUnmatchedRemoveDeltas=0`, 11,960 acked blocks survived) but failed 2/28 of its own strict verdicts on request-timeout cascades traced to a compromised host (still recovering from a prior `--scale full` attempt, load average 22-48). Not evidence of a defect, but not a certification either — re-run on a quiet host before calling S42 green.


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

- **[ckpt-neverborn-gc-backstop] GC-level backstop needed for never-born `_ckpt` debris** — {#ckpt-neverborn-gc-backstop} — HARD — Task 4-C removed all three `cleanupOrphanedBirthCkptBestEffort` call sites: each sat on the `CORRUPTED_DATA` path, which by `putIfAbsentControlled`'s own contract means the ref-log at that key is non-empty — contradicting the delete's own "never durably held anything" justification, so a successor's live `_ckpt` could be deleted unrecoverably. Correctly removed rather than guarded (a fallback must never take a destructive action). Traded for: permanent debris that makes a drained server root refuse decommission (`claimOwnerOrThrow` → `CORRUPTED_DATA`), operator-visible. The needed backstop, mirroring the existing REMOVED-namespace `_ckpt` backstop in `CasGc.cpp`: reclaim a never-born namespace's `_ckpt` only after independently re-verifying emptiness with a real LIST, never by inferring it from one attempt's own conflict.

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

Measured 2026-07-31: every table creation in a pool writes the same catalog object
(`cas/ref_catalog`), so a lane creating thousands of tables serializes them all through one CAS loop
— 137/250 S3 timeout lines on the CA-s3 stateless lane named this one key. Not evidence the catalog
design is wrong (it exists because pool `LIST` is unreliable), and the measurement is from a lane
that creates tables far faster than any real deployment — but a genuine cost the design didn't
expose before. **Open questions before a fix**: does the write rate come from creation only, or also
from read-mints; is the retry deadline just too short for the contention it now sees; can the object
be sharded/batched without giving up the single-object atomicity the GC universe snapshot needs.

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
