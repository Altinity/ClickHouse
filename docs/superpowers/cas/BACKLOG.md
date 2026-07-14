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

## 3. GC correctness / observability follow-ups {#gc-followups}

- **[GC round progress observability] round-duration watchdog + fold-window events** — HARD — A long/wedged round is only visible after the fact; emit a round-duration watchdog, LIST/fold-window progress events, and an alert on an unbalanced `gc_fold_begin`/`gc_fold_end` pair.
- **[ack-floor soak validation] 3 scenario cards** — TEST — SIGSTOP-a-writer holds-then-releases the floor; hard-KILL-writer → fence-out → fsck no-dangle; O(delta)+O(servers) request-count regression guard. Note: floor semantics changed under freshness-v3 (round-paced, per-hash `.meta`) — update the cards. Implemented + unit/TLA-covered; not soak-validated.
- **[F3] `ca-gc-dryrun` reachability under-counts vs real GC/fsck** — HARD — Systematic across S18/S25/S26/S33; real GC always safe (dangling=0). Fix: dryrun uses the SAME reachability walk as GC/fsck; assert `dryrun ⊆ (unreachable ∪ pending-gc)`.
- **[D3 / S31] full GC round + `ca-gc-dryrun` completeness under `gc_shards>1`** — TEST — `gc_shards=1` tests hide sharded fold bugs; cover fold→retire→reclaim over the source-edge set with multiple GC shards.
- **[clamp liveness] scoped suppression under long persistent clamps** — DESIRABLE — Fail-closed clamp+suppression is correct and self-heals (false-404 attribution), but suppression-vs-liveness under long clamps is unaddressed (scoped suppression later). Clamp observability (clamped key/shard event) is DONE.
- **[gc-rebuild follow-ups]** — MINOR — Dedicated gc-round-log row for `rebuildBaseline` (currently only `LOG_INFO` + a `gc_rebuild` event); the "unowned-alive manifest edge over-protect" documented leak (bounded, fsck-visible, cleared by a future rebuild); soak validation (`mc rm gc/state` mid-soak → guard fires `CORRUPTED_DATA` → `SYSTEM … GC REBUILD` recovers to dangling=0).
- **[fsck oracle gaps]** — MINOR — fsck under-reports orphan manifest bodies for ref-less namespaces (enumerate `cas/manifests/` too, not just `cas/refs/`+`roots/`); fsck Orphan-class test gap.

## 4. Read / write path {#read-write}

- **[B121 / B202 / one-GET-open] read request-count reduction** — DESIRABLE (design pass) — B202 inline-by-size (drop the file-type predicate, inline < ~512 KiB, weigh the wide-part-medium-column regression, `.bin` carve-out) + a per-blob-GET read-cost reduction (B121) + one-GET part open (pack small files). Pure perf/request-count; no safety dimension. Companion to the (landed, opt-in) file-cache disk for re-read-heavy workloads.
- **[B10] `manifest_size` always 0 in `Resolved`** — MINOR — `Store::resolveRef` hardcodes `.manifest_size = 0` (`CasStore.cpp:844`, twin at `:995`). Now a **live consumer**: `PartFolderView::weight()` = `bytes + manifest_size`, so the part-folder cache weight silently under-counts the manifest body. Set it.
- **[B98] Streaming `putOverwrite` (condemned-displacement)** — DESIRABLE — The rare INV-1 revival/displacement path still materializes the whole body; not a blocker.
- **[promote-recreate] promote-time in-place recreate of a condemned SOURCED (tokened) blob** — DESIRABLE — The tokened promote gate stays fail-closed `ABORTED`; recreate happens on the retried build via `putBlob` cold-reuse. The tokenless-evidence copy-forward case is DONE. Ideal root-cause fix (writer-triggered synchronous fold-barrier at promote) is blocked by the lack of a writer↔GC synchronous-fold API — deferred behind the landed bounded resurrect.
- **[R1/X1] ephemeral reader pin (cross-node GC fence)** — DESIRABLE / VERIFY — Per-server-owned namespaces narrow the window and a live ref resolving to an absent object surfaces `FILE_DOESNT_EXIST` (INV-NO-DANGLE), so for normal MergeTree this is covered by DataPart lifetime; the ephemeral-pin mechanism is design-only. Audit whether any ref-less/cross-node reader path exists before implementing.
- **[B85] read-path 404 auto-repair** — VERIFY — Read-path 404 currently surfaces as a hard error (INV-NO-DANGLE). Auto-repair-on-404 was an open resilience idea; confirm whether still wanted post-v3.
- **[ch128ctx] slot-bound blob-hash middle tier** — DESIRABLE (small spec) — New `BlobHashAlgo` variant: `cityHash128(content) ∥ xxh3_64(part_name, file_name) ∥ size` (256-bit; variable-width `BlobDigest` already supports it). Binds blob identity to its minting slot, so *cross-slot* collisions (the realistic adversarial dedup vector: attacker-crafted content deduped into a victim's future blob) become useless, at ~zero CPU cost over `cityHash128`. Every load-bearing dedup survives: relink/carry-forward are reference-based; retry idempotency, same-name replica writes, and snapshot-upload→TTL-move prepayment are same-slot; only cross-slot content coincidence is lost (an explicit non-goal, `01 §what-it-does-not-buy`). Middle tier of `cityHash128` → `ch128ctx` → `sha256`. Main touch: the hasher interface needs `(part_name, file_name)` context injection into `putBlob`. Origin: backup manifest-reuse discussion, `10-backups.md §multi-disk` (2026-07-14).

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

- **[B1] `manifest_hash` on the Keeper `/parts` znode** — GATE — Cross-replica header-divergence detection; `commitPart`/`getCommitPartOps` in `ReplicatedMergeTreeSink.cpp` have no CA-specific field yet. Relink base is DONE.
- **[B197] `SYSTEM` control surface — START/STOP GC, POOL READONLY, CHECK** — GATE — Product-side GC stop is currently only a soak-harness workaround.
- **[B198] backup/restore runbook** — GATE.
- **[B180 / format-freeze] pool-format version breadcrumb + first-release format freeze + rollout machinery** — GATE — Stamp the pool self-describingly; freeze the format on the first persisted-data release (schema-evolution framework is in place); durable roster + `max_content_addressable_pool_format` setting/rollout machinery not built (Part IV).
- **[B125] integration tests on RustFS (not MinIO)** — GATE — Current integration tests use MinIO; production uses S3-compatible backends that enforce conditional-PUT.
- **[B131] repo hygiene + M-W comment sweep** — GATE — 30 dangling `M-W`/`D-W1`/`2026-06-12-ca-core-m-w` comment references across 13 src files (incl. `ContentAddressedMetadataStorage.{h,cpp}`, `CasGcScheduler.h`, `DataPartsExchange.cpp:106`) reference the deleted plan — sweep to self-contained wording. Non-shippable files: `poc/cas_mergetree/` already deleted (F1 landed); the untracked empty `poc/` husk remains.
- **[B15/B99/B169/B159] `system.*` views for pool/blob/part refcounts + `clickhouse-disks` decode/introspect** — HARD (PARTIAL) — GC log + event log + `content_addressed_mounts` + fsck/dryrun/rebuild/ca-inspect CLI done; per-part/ref `system.*` views + a top-down decode/traversal surface not yet. (INTROSPECTION-1/2 close signals.)
- **[B13] migration path for existing tables** — HARD — `ALTER TABLE … MOVE PARTITION` to a `content_addressed` disk re-packs; mixed-version rollout rule (read-new-before-write-new; format self-check fails closed) + a rollout-safety spec.
- **[B48 / B167a/f] clean shutdown** — GATE — `clickhouse local` + CA disk hang (GC thread / `BackgroundSchedulePool` not reaped on `LocalServer` exit); server-side graceful-shutdown ordering (stop scheduler → release lease → farewell beat).
- **[F1-prod] read-only same-pool shadow disk (`ca_ro`) breaks table load on restart** — GATE (prod) — MergeTree part discovery finds every part twice → `UNKNOWN_DISK` on restart with CA tables. Stand workaround shipped (standalone `clickhouse-disks -C` fsck-only config; propagated to the default stand); PRODUCT fix (part discovery skips `readonly` same-pool disks, or a `hidden`/`introspection_only` disk flag) still open; `10replicas`/`gc_shards2`/`awss3` server configs may still embed `ca_ro`.
- **[B165] server OOM at hour-4 soak (~49 GiB RSS)** — VERIFY — Not reproduced since the `putBlob` streaming fix; re-run a long soak to confirm resolved.
- **[SEC-1] trust-domain documentation** — DOC — Document "one CAS pool = one trust domain" (CityHash128/XXH3 are not cryptographic; dedup is cross-tenant within a pool). For a multi-tenant future: crypto hash mode or trust-domain-scoped dedup (sha256 mode now exists as a building block). SEC-2/SEC-3 are by-design under this model.
- **[AD-3] day-2 runbook** — DOC — Table of failure mode → signal/metric → diagnostic command → recovery command → test (stalled GC, persistent clamp, lost/corrupt `gc/state`, live mount conflict, orphan refs/manifests, pool-meta corruption, control-plane backup/restore).
- **[B14] expedited / GDPR right-to-erasure delete** — DESIRABLE — Under GC lock, confirm no live ref, then delete bypassing the two-phase graduation delay; no layout change.
- **[B17] encryption-at-rest × content-addressing** — DESIRABLE — Dedup scope per-encryption-key; local to key/hash derivation.
- **[B26 / B135] local / NFS / shared-fs as a first-class backend** — DESIRABLE — Unit-tested over `LocalObjectStorage`; needs server-level doc + the put-if-absent atomicity caveat (racy multi-writer on local/NFS) + multi-mount safety notes. (B66a concurrent-fetch torn read on local is the concrete instance.)
- **[B66a] concurrent-fetch torn read of shared `detached` ref on local storage** — MINOR — `LocalObjectStorage` write is not atomic; safe on S3 (atomic PUT).
- **[B66b] relink-into-detached (zero-byte `to_detached` fetch for same-pool parts)** — DESIRABLE — Currently byte-streams; extend `Fetcher::relinkPartToDisk` to honor `to_detached`. (RPL-4 perf cliff.)

## 8. Mount-lease / fence recovery {#mount-fence}

- **[P3.1 Task 6 / S13] live validation of fence-recovery** — TEST — TLA+ gate PASSED and the correctness paths landed (self-remount on GC fence-out is DONE); the gtest sweep + S13 3×-green live gate remain. **Task 5** (decouple renewal from the retired-view sync beat) is likely **MOOT** — freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`; confirm and close.
- **[A7-residual] gc_scheduler lifetime vs manual rounds** — VERIFY — Believed addressed by `89845c2a544` (shutdown serializes gc_scheduler teardown with health reads; wedged-lane count pinned) on top of the stabilization A7 fix. Confirm no residual: (a) a manual round on a raw pointer captured outside the lock, (b) lazy creation resurrecting a scheduler after shutdown.

## 9. Architecture / refactoring (deferred, no behavior change) {#refactoring}

- **[refactor: CasGc split] break `CasGc.cpp` into workflow units** — DESIRABLE — Split scan / reachability / deletion / cursor / budget out of the 2.3k-line file; keep `Gc` as orchestration (pure extraction). Author's second-highest-value refactor. (review1 #13; refactoring-ideas #3.)
- **[refactor: Store de-god-classing] extract remount-thread / caches / ref-append-lane out of `Cas::Store`** — DESIRABLE — 8-responsibility god class; friend-triangle with `Build`/`Gc`. (review1 #13.)
- **[refactor: Store::open modes] split into create / open-rw / open-ro** — MINOR (real bug behind it) — Read-only `Store::open` can still write `_pool_meta` on an empty pool (`PoolMeta::createOrValidate`); make read-only semantics visible (`createOrLoad` vs `loadExisting`) or pass `create_if_missing=false` when `read_only`. (refactoring-ideas #1.)
- **[refactor: DiskObjectStorageTransaction part-path virtualization]** — DESIRABLE — Push eager-dispatch behind an `IMetadataTransaction::requiresEagerDispatch(from,to)` virtual so the generic transaction layer stops `#include`-ing `PartPathParser.h`/`ContentAddressed`. Deferred from stabilization (§6) to avoid growing shared Ring-2 surface. (review1 #13.)
- **[DiskSelector per-disk isolation]** — HARD / upstream — `DiskSelector::initialize()` has no per-disk try/catch; one unreachable disk aborts disk-selector init server-wide. Pre-existing upstream gap; carve to an upstream PR (Group G). (review1 #5 residual.)
- **[emulated list-token contract]** — MINOR / VERIFY — `ObjectStorageBackend::list` in `EmulatedSingleProcess` mode may still return a different token kind than `head`/`get`/`put`/`delete` (child etag vs `emuObserveToken`), a Liskov gap vs `supportsListTokens`. The token-policy centralization (C1, landed) added `tokenForHead/tokenForList/tokenMatches`; verify the emulated `list` path now agrees or make `supportsListTokens()==false`.
- **[Group G] carve generic Ring-2 fixes into separate upstream PRs** — MINOR (fork hygiene) — Shrinks the fork's long-term conflict surface: `ThreadStatus parent_thread_group` (B90), `ReadBufferFromFileView` (B115), `ReadBufferFromS3` cancel-stop (B117), `LocalObjectStorage` TOCTOU (B38), `MergeTreeDeduplicationLog` null-writer (B37), `copyS3File message_format_string`, `Expect:100-continue` opt-in, `S3Exception::isPreconditionFailed`, GCS conditional dialect + GOOG4 signer, generic conditional-S3-write plumbing. Non-blocking.
- **[weighed refactors — not scheduled]** — see `refactoring-ideas.md` for the WEIGHED-not-mandated set (typed key-wrapper helpers #5, codec-validation/workflow split #6, naming disambiguation #10, backend-test fixture DSL #9). Token-policy (#2) + list-pagination (#7) + delete-outcome classifier (#8) already landed (C1/C2).

## 10. Test coverage & harness {#tests}

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

## 11. Scalability findings from the full-scale campaign (S3 budget) {#scale-findings}

These are real scale/budget findings; most are variants of "O(N) GC / per-op amplification". Track for the capacity model + a future S3-budget push.

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

## 13. Minor / polish {#minor}

- **[Issue-6] B3 GC-health columns denormalized onto non-local mount rows** — MINOR — The four `GcHealth` columns are stamped identically on other servers' mount rows (`StorageSystemContentAddressedMounts.cpp:159-162`); NULL them on non-local rows.
- **[F4] CA `MOVE PARTITION` publishes ref CAS before validating the target disk is in the storage policy** — MINOR — S19: 2 `CasRootCas` ops during a correctly-rejected `UNKNOWN_DISK` move (safe, dangling=0); validate the destination disk in-policy before any ref CAS.
- **[snappatch-minor] `CasStore.cpp:2007` replay throw escapes `trySnapshotPublishOnce` without arming publish backoff** — MINOR (defensive) — Dead today by the `min(tail)>newest_snapshot_id` invariant; defensive-pass candidate (likely removed anyway by rev.6).
- **[Build::promote owner-liveness guard is race-only]** — MINOR — Fires only in the narrow promote-vs-dropNamespace window; make the race deterministically testable or remove the guard with a TLA argument.
- **[Ring-2 comment/convention nits]** — MINOR — `S3Common.h` comment overclaims for the RetryStrategy site; `static_assert(DEFAULT_EXPECT_CONTINUE_MIN_BYTES==0)`; `MergeTask::projection_uses_parent_transaction` could be a local; `ProfileEvents.cpp` changelog fragment in a description; `_ms` suffix on a `DateTime64(3)` column; the new `GC REBUILD` right abbreviates "GC" vs the sibling spelled-out "GARBAGE COLLECTION"; internal `cas_part_folder_cache_*` names outlived the key rename; `05011` `no-parallel` tag droppable; empty untracked `poc/` dir husk.
- **[C2-followups] more pagination loops for `forEachListedKey`** — MINOR — Three more identical loops in `CasRefIntake.cpp`/`CasServerRoot.cpp`; `forEachListedKey` also lacks a stop-on-true/page-boundary hook to let `deletePrefixWholesale` + ns-cleanup migrate (interface addition, design first).

## Recently closed (2026-07-13 grooming — do NOT re-open) {#recently-closed}

Verified DONE at HEAD; recorded so they are not re-triaged:

- **Whole stabilization iteration** (`docs/superpowers/plans/2026-07-12-cas-stabilization-cleanup.md`) — every task committed (A1 `~Store()` teardown abort, A2 RunFile OOB, A3 flushRefBatch wedge, A4 EDGE-BEFORE-OBSERVE throw, A5 Ordinary-detached namespace, A6 skip_access_check, A7 stable-Gc manual round, A8 fold-seal enum range-check, A9 dropRefBestEffort logging, A10 suppress_destructive once; B1 path memoization, B2 explain-journal opt-in, B3 per-disk GC health, B4 late-predecessor counter [→ to be removed by rev.6]; C1 token policy, C2 forEachListedKey + delete classifier, C3 blobKey in CasLayout.cpp, C4 unified dir dispatch, C5 whole-part-txn encapsulation; D1–D5 dead-code/vestigial removal; E1 GC-REBUILD access right split, E2 config naming, E3 typed mounts columns; F1 delete `poc/cas_mergetree`; R412 one 412 policy, RExpect scoped 100-continue).
- **Umbrella review `review1.md`** — findings 1, 3–12 + minors fixed by the stabilization iteration; finding 2 (relink "RBAC") retracted as not-a-bug and documented (interserver channel == ordinary `ReplicatedMergeTree` trust boundary); only findings 13 (god-class/virtualization refactors) and 14 (coverage gaps) + `DiskSelector` isolation carried forward (§9, §10).
- **B31** capability gate — `supportZeroCopyReplication()==false` for CA with a B31 comment; unsupported ops rejected by independent gates (ALTER PARTITION throws, BACKUP restore routes through a whole-part transaction). **B192** event-name review — 51 event types all neutral snake_case, no flagged terms. **B10 minors** `~Build`/`retireBuildSeq` (no I/O on the dtor path), `inDegreeInGeneration` (test/preview-only), redundant `sweepNamespace` watermark GET, signed-in-degree accumulation (unsigned edge-set model) — all fixed. `RunFileReader::seek` FIRST-block-≥-target contract bug fixed (`035edbcf7e1`). Vestigial manifest-backpressure surface removed (`e743da297bb`). B207 fsck phantom-dangling, B3/B186 `FreezeViaHardLinks` red, deposed-leader `clearSparedMeta` — all RESOLVED (2026-07-11).

## Obsolete / superseded (removed or to be removed) {#obsolete}

- **Root-shard-axis items are MOOT** — the mutable `RootShardManifest` / per-`(ns,shard)` root-shard ref model was **replaced by the per-table snapshot+log** (`PoolMeta.root_shards` deleted, `Store::shardOf` gone). The following ROADMAP items no longer apply: **per-namespace `root_shards`** (DESIRABLE), **adaptive shard SPLITS** (DESIRABLE), **root-shard fan-out vs per-object permit cap (B158)**, and the **shard-mutation flat-combining queue** (`specs/2026-07-03-cas-shard-mutation-queue.md`; superseded by the `CasSingleWriterSlot` per-table ref-append lane — its `CasShard*` ProfileEvents were removed in stabilization D2). **B111** (`RENAME` = one Build/part, "multiple root-shard updates per rename") is likely moot/reframed under the per-table ref log — revisit only if a rename cost shows up.
- **`CasRefLatePredecessorObserved` (B4)** — landed (`10274550bb3`), to be removed with rev.6 (grace-window machinery goes away). See §1.
- **`deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`** — SUPERSEDED+IMPLEMENTED (A1 ranged reads + A3 streaming `readPriorEdges` landed via T2/T0); only the T1 delta-runs residue remains, tracked in §2. File removed in this grooming.
- **Superseded specs/plans (kept for history, banner-marked):** `specs/2026-07-10-cas-ref-snapshot-log-design.md` (GC-owned-base model → superseded by the 2026-07-11 writer-owned rev.5); `plans/2026-07-10-cas-meta-descriptor-raw-body.md` (REJECTED — recreated the rejected generation-in-key model; Phase B landed as freshness-v3 instead); `plans/2026-07-09-cas-promote-resurrect-tokened-blob.md` (landed then removed by writer-GC-simplification Phase A / EDGE-BEFORE-OBSERVE); `specs/2026-07-01-cas-shard-incarnation-and-registry-removal-*` (registry removal DONE via D1; RootShard incarnation moot under snapshot+log).
- **P3.1 lease-view-sync-decouple Task 5** — likely MOOT: freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`, so "decouple renewal from the retired-view beat" has no beat to decouple from. Confirm + close (§8).
