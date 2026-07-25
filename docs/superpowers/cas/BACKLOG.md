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

---

## 1. Ref protocol — rev.6 lease-boundary exclusivity (highest-priority open design) {#ref-protocol}

- **[rev.6] Lease-boundary exclusivity — remove the grace window + publish-path replay** — HARD (user-driven) — Proposal `specs/2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md`, **awaiting user review + TLA+ gate before implementation**. Solve writer exclusivity once at the mount-lease handover: unclean-handover wait (`materialization_grace_ms`/`T_mat`), `released_clean` clean-unmount fast path, eager recovery-snapshot **seal** (mount writable only after it commits), publish-from-live snapshots. Deletes `snapshot_min_log_age_ms` + the per-entry `RefTableState` replay. Amendment checklist 1–7 + open questions Q1–Q4 (`T_mat=30s` acceptable? eager snapshot on clean large-tail mounts? hard-require wedge/single-in-flight at the lane?). Interim mechanical patch already landed (`3c7003ce190`: aged+uncovered trigger, copy-once replay, threshold 64→256).
- **[Late Predecessor PUT] cross-epoch late-materialization correctness limitation** — HARD — The hazard rev.6 closes: a fenced predecessor's in-flight PUT can materialize below successor snapshot coverage (a missed `−1`/`+1` = data-loss class). Phase-1 documents it; the real fix is the rev.6 seal (or a Keeper-based cross-epoch fence). **OBSOLETES `CasRefLatePredecessorObserved` (B4)** — that counter landed (`10274550bb3`) then is slated for removal by rev.6; remove it when rev.6 lands.
- **[refsnaplog Phase 2] measured ref-log/snapshot optimizations** — DESIRABLE (measurements-gated) — inline zero-byte log keys; GC-side fallback compaction for never-mounted tables; indexed/chunked multi-object snapshots; lazy snapshot blocks + byte-bounded row cache; per-round ref index; streamed snapshot construction; adaptive thresholds; decoded-body reuse; chunked namespace removal. Plus a **cross-epoch fault-injection integration test** reproducing the late-predecessor counterexample.
- **[timeout-retry RFC residuals] bounded lease-aware S3 timeout/retry controller** — PARTIAL — `CasRequestController` (single-attempt conditional writes, budget, fence-gating, exact-key resolution) landed for the ref lane. RFC `specs/2026-07-12-cas-s3-timeout-retry-control-rfc.md` residuals still open: (a) AWS SDK region-redirect retry can bypass `ShouldRetry` when a client is `aws-global` (CAS disks are not aws-global today — add a startup guard/probe if that changes); (b) `promoteStaged`'s `copyObjectConditional` (server-side conditional copy) is a separate conditional-write mechanism NOT bounded by the single-attempt work — verify its retry semantics before relying on write-once promote; (c) bounded read/HEAD/LIST retries + startup validation for the non-ref plain-object paths (`casPutObject`/`casRemoveObject` still use the disk's default retry policy).

## 2. GC scalability & byte cost {#gc-scalability}

- **[T1] Delta-runs + compaction / incremental (LSM) snapshot** — DESIRABLE — The single dominant remaining byte cost: a HOT pool rewrites the full snapshot run O(edges) per pass. Build O(delta)-write log-structured runs + periodic compaction on the landed T2/T0 primitives (streaming reader, `seek`, `getStream`, ranged `get`, seal-ref resolution). Canonical item (dedups: snapshot-streaming T1, ack-floor T1, 04/07 rows, refactoring-ideas "incremental LSM snapshot", O(buffer) run-file streaming residue). Streaming reads + reference-parent runs (T2/T0) already DONE.
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
- **[clamp liveness] scoped suppression under long persistent clamps** — DESIRABLE→HARD (2026-07-18: concrete reproducer) — Fail-closed clamp+suppression is correct and self-heals (false-404 attribution), but suppression-vs-liveness under long clamps is unaddressed (scoped suppression later). Clamp observability (clamped key/shard event) is DONE. NEW: S38 demonstrates a poison late log clamps its own key and thereby starves `reportLateLogsIfAny` indefinitely — 40 healthy Success rounds, sweep pass suppressed in every one, `RefLateLogDetected` never fires (`2026-07-18-s38-late-log-clamp-starvation.md`). Minimum fix: the clamp path itself must emit the late-log/clamped-key report even while the sweep skips processing.
- **[UNMATCHED-MINUS-ONE] unmatched removal-fold `-1` — VERIFIED HARMLESS (edge-set model); pin the property** — TEST (was briefly filed as HARD data-loss, 2026-07-23; corrected same day) — The suspected interleaving is real up to its last step: an ordinary `Precommit`'s activation-fold can hit a false-404 on a durable body (rustfs#3231 class) while the build is provably dead → dead-build skip eats the `+1` (`CasGc.cpp:1142`); the successor's stale-precommit sweep later appends the removal (`CasRefLedger.cpp:1958-1970`); the removal-fold reads the now-readable body and emits `-1` edges that were never `+1`-folded. BUT the terminal "in-degree under-count → premature delete" step does NOT fire: in-degree is a **source-edge SET, not a counter** — "Idempotent under re-fold at the merge (set membership, not a counter)" (`CasBlobInDegree.h:139`), applied last-wins per edge key (`CasBlobInDegree.cpp:574`, `:383-384`), and edge keys are per-(ref, ManifestId, path) — so an unmatched remove marks an already-absent edge absent and CANNOT touch другие manifests' edges. Consequence: harmless debris (orphan body + a no-op event), no data loss. This also refutes the terminal step of the codex finding-1 counterexample against the (abandoned) reserved-precommit design — that design was dropped for its OTHER confirmed problems (state-model sprawl, foreign-writer stageManifest bypass, tmp-fetch contract mismatch). REMAINING WORK: pin the load-bearing property with a cheap gtest (fold an unmatched `-1` and assert sibling edges survive + no condemn) so a future model change (set → counter) cannot silently reintroduce the class; optional TLA note REMOVAL-IS-SET-ERASE.
- **[gc-rebuild follow-ups]** — MINOR — Dedicated gc-round-log row for `rebuildBaseline` (currently only `LOG_INFO` + a `gc_rebuild` event); the "unowned-alive manifest edge over-protect" documented leak (bounded, fsck-visible, cleared by a future rebuild); soak validation (`mc rm gc/state` mid-soak → guard fires `CORRUPTED_DATA` → `SYSTEM … GC REBUILD` recovers to dangling=0).
- **[fsck oracle gaps]** — MINOR — fsck under-reports orphan manifest bodies for ref-less namespaces (enumerate `cas/manifests/` too, not just `cas/refs/`+`roots/`); fsck Orphan-class test gap.
- **[repointRef non-resolving-key audit gap]** — MINOR — `CachedPartFolderAccess::repointRef` (`CachedPartFolderAccess.cpp:283`) increments `CasRefRepoint` and logs "Repointed committed ref…" unconditionally after its `if (resolved)` byte-equal check, even when `resolve(key, ForceFresh)` returns `nullopt` — i.e. it would count/log a repoint for a key with no existing committed ref. Unreachable today (every caller — Task 4's standalone writes, Task 8's removal-mark resolution — only calls `repointRef` on an already-resolving key); a defensive `throw LOGICAL_ERROR` on `!resolved` would make the precondition explicit and the counter/log trustworthy rather than merely-currently-true. (Found during all-tree Tasks 7/8 integration review.)
- **[ProvenanceOp operability gap]** — MINOR — Both the Task 4 committed-ref standalone write and the Task 8 removal-mark repoint call `repointRef(..., Cas::ProvenanceOp::Other)` — no distinct op kind for a removal-repoint vs a write-repoint in `system.content_addressed_log`. Spec doesn't require one; would help an operator distinguish "this repoint dropped files" from "this repoint added/changed files" in the audit trail without decoding the entry diff. Product-owner call, not decided during Task 8 integration. (Found during all-tree Task 8 integration review.)
- **[codex-11] namespace drop misses an unregistered build → ownerless Live namespace** — LOW — `Pool::beginPartWrite`'s allocate/register window (`CasPool.cpp:772-777`) is real: a build can allocate before the drop sweep (which only snapshots `inflight_builds`) runs, then legitimately pass the birth-time marker gate afterwards, reviving a Live-but-ownerless EMPTY ref-table — a small, non-self-healing metadata leak (GC never sweeps Live namespaces). Confirmed narrow, LOW severity (2026-07-17 codex-review triage, finding №11); the reviewer's atomic-registration fix would NOT close it (the same TOCTOU recurs between the `cancelled` check and the append for already-registered builds). Fix direction: a GC backstop that reclaims empty ownerless Live namespaces, or a namespace generation folded into the birth-time marker gate.
- **[RECOVERED-INDEGREE-ATTRIBUTION] move the "delete_pending recovered in-degree" invariant check to the writer** — DESIRABLE (2026-07-24, PR#2073 CI triage) — The GC-side `LOG_WARNING "…recovered in-degree — structurally impossible under the ack floor; investigate"` (`CasBlobInDegree.cpp:418`, `CasGc.cpp:485`) fires as a *false alarm* (56×/run on tiny system-log blobs in the CAS-s3 stateless job): root-caused to a **dedup-adopt-vs-condemn TOCTOU** — a write-once PUT hits `PreconditionFailed`, `observeAndAdmit` (`CasPartWriteTxn.cpp:354-421`) meta point-reads the blob as not-Condemned (read preceded GC's `Condemned` write) → adopts the token → the fresh edge recovers in-degree after GC's fold cut → GC **spares** (verified no data loss: zero read-side blob 404s). By construction a `delete_pending` blob carries no surviving prior edges (`CasBlobInDegree.cpp:518-521`), so every such recovery is a fresh this-generation edge — GC cannot locally tell a legitimate race from the ONE genuine bug this masks (**adopt-without-resurrect**). Fix direction: (a) downgrade the GC log to a dedicated `ProfileEvent` (`CasGcRetiredSparedByReref`) + `LOG_DEBUG` (metric already exists: `CasGcRetiredSpared`, `CasGc.cpp:503`) so the false alarm and the misleading "structurally impossible/investigate" wording go away; (b) put the real detector where it can decide — at the writer's edge-commit, re-check the meta (or record the meta-generation the adopt was based on) and emit a typed `BlobAdoptRacedCondemn` event (with `source_id`) iff the blob became Condemned between observe and commit; then the GC spare is a silent safety-net. Also enrich any remaining signal with the recovering `source_id`+round (see [[reference_cas_ci_observability_gaps]] #4). RCA in [[project_pr2073_ci_triage_2026_07_23]].
- **[CONDEMN-GRACE-WINDOW] cool-down before condemning a just-zeroed blob (kill hot-dedup churn at the source)** — DESIRABLE (2026-07-24, PR#2073 CI triage) — The driver of the RECOVERED-INDEGREE noise (and its wasted condemn→HEAD→spare→re-condemn cycles) is tiny system-log blobs (`system.content_addressed_log`/`trace_log`/`zookeeper_log`, 286–415 B) whose in-degree hits 0 and is re-referenced by dedup almost immediately. A short grace/cool-down before graduating a blob whose in-degree just transitioned to 0 (defer condemn by one round or a few seconds) lets the re-reference land as a surviving edge, so the blob is never condemned — removing the spare churn AND the associated HEAD-storm/S3 request cost on hot blobs (ties to the `07-s3-budget` request-count model and [[project_cas_insert_slowness_writepath]]). Higher risk: this changes condemn timing → touches GC invariants (retention vs reclaim latency, ack-floor interaction), needs TLA-level reasoning and is subject to the protocol-step change veto ([[feedback_head_before_put_protocol_untouchable]]) — do only as a deliberate design with a model, not a "cheap" tweak. Measure first: count re-condemn/HEAD/spare cycles on the 28 hot hashes from run 30019911967 to size the benefit. RCA in [[project_pr2073_ci_triage_2026_07_23]].

## 4. Read / write path {#read-write}

- **[write-path stage 1] parallel intra-part blob upload — LANDED (2026-07-24)** — The single-threaded serial blob-upload bottleneck of the wide CAS-on-S3 INSERT (documented in [[project_cas_insert_slowness_writepath]] and the point-4 baseline `docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md`) is now addressed by write-path stage 1: a server-wide `CasBlobUploadPool` + `fanOutBlobUploads` fan out a part's blob PUTs/dedup-HEADs (spec `docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md`, plan `docs/superpowers/plans/2026-07-23-cas-writepath-stage1.md`). Re-profile (`docs/superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect.md`): CA wide-insert wall **58.41s → 30.26s**, CA-vs-plain **3.0x → 1.59x**, top-thread Real share **72.3% → 14.5%**, CPU/wall **0.375 → 1.075** — the single-threaded signature is gone. Residual gap = the serial cross-part commit (ref batch still 1.0 BY DESIGN → **stage 2**, concurrent `commitPart` dispatch) plus the CAS-only dedup HEAD/GET traffic (~12% of Real wall — see `[B121 / B202 / one-GET-open]` below and the HEAD-before-PUT gate). Stage 2 is the next lever, now scoped against a smaller residual than the original 3.0x framing.
- **[TXN-ONE-PIPELINE] complete the "staging ops never defer" invariant** — HARD (small, structural) — The `01603` column-TTL abort (`de8a38b1e87`) was an ordering inversion between `DiskObjectStorageTransaction`'s two dispatch pipelines: eager (CA staging ops: `writeFile`/`createHardLink`/`moveDirectory`, and now part-file unlinks) vs deferred-to-commit (durable ops). A total order is impossible (read-your-writes B58/B63 and B151 force pre-commit effects; abort safety forces commit-gated durable deletes), so the correct invariant is *per-state-domain*: EVERY op that touches in-memory part staging must dispatch eagerly, leaving the deferred queue exclusively for durable non-staging effects (the two domains commute, closing the inversion class structurally). Residual gaps: `moveFile`/`replaceFile` in the part-file→part-file shape are still deferred (their B182 eager hook was deleted in T6 as trigger-less — the same "no trigger today" state `01603` was in before T8). Target shape (user direction 2026-07-15, option B): the deferred queue CEASES TO EXIST for CA — durable effects become staged INTENTS too (verbatim-file delete intent, part ref-drop intent, mountpoint delete intent) materialized at commit, and CA gets its own `ContentAddressedDiskTransaction` subclass dispatching every op straight to the metadata transaction in program order, deleting all four per-method `isContentAddressed()` branches from the base class (each of which was added through a bug: writeFile, createHardLink B58/B63, moveDirectory B151, unlink gate `de8a38b1e87`/`725dbc7d83c`). Delicate parts: verbatim writes are durable-immediate today (staged-write unification touches the mutation-MVCC read-modify-rewrite append) and autocommit one-shots keep their individually-durable contract. REFINED (user, 2026-07-15 evening): pair it with an explicit TWO-PHASE disk-transaction contract — new `IDiskTransaction::precommit()` (noop by default, zero change for ordinary disks); CA precommit = the ENTIRE publish (manifest from the overlay -> `precommitAdd` -> upload missing blobs -> promote), called before ZK-multi in Replicated (next to `renameParts()`) and before the `data_parts` lock in plain `Transaction::commit`; CA commit = durable-intent materialization only; abort-after-precommit = today's `dropRefBestEffort` compensation in an explicit phase. `moveDirectory` stops publishing (pure staging re-key) — B151's rename-window publish and the `rename_published_refs` machinery are DELETED. Verified basis: parts are `PreActive` until disk commit (no owner reads), the fetch handler serves `PreActive` but vanilla plain-s3 already fails+retries such fetches (queued rename), so the announced-but-unpublished window is an accepted upstream race, NOT a protocol obligation — the earlier 'publish must precede ZK announce' rationale was overstated. Contract decisions (user-settled 2026-07-15): `commit` IMPLICITLY runs `precommit` when it was not called (idempotent, flag-guarded — classic commit-implies-prepare; an assert would turn a positioning omission into a prod abort across dozens of commit call sites incl. the Keeper-recovery branch), WITH observability (ProfileEvent `CasImplicitPrecommitInCommit` + debug log) so a mis-positioned hot path surfaces in soak metrics; staging mutations arriving AFTER `precommit` are a genuine correctness violation -> fail-loud `LOGICAL_ERROR`. SCOPE ADDITION (user 2026-07-15): the refactoring must also DE-PATCH upstream code — remove the accumulated workarounds around eager-dispatch/read-your-writes in non-CA files; inventory with A/B/C classification + de-patching order = `docs/superpowers/cas/upstream-patch-inventory.md`. SPEC: `docs/superpowers/specs/2026-07-15-cas-txn-one-pipeline-design.md` (2026-07-15, approved brainstorm) — SUPERSEDES the staged-intents wording above: everything-immediate model (no intents — ABA rejection; local-disk abort semantics for verbatim/mountpoint deletes), single `dispatch` funnel in `DiskObjectStorageTransaction` (no CA subclass), generic `tryCreateWriteBuffer` hook, `precommit` call sites in `renameParts`/plain `commit`/freeze/restore/fetch; lands BEFORE codecs v3 and the source-layout refactoring.
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
