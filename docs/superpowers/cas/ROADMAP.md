---
description: 'Consolidated cross-area DONE / TODO / REJECTED / DESIRABLE roll-up for the CAS MergeTree feature. A single place to see the whole feature state, linking into the section documents.'
sidebar_label: 'CAS Roadmap'
sidebar_position: 10
slug: /superpowers/cas/roadmap
title: 'CAS MergeTree — Roadmap and Status Roll-up'
doc_type: 'guide'
---

# CAS MergeTree — Roadmap and Status Roll-up {#cas-roadmap}

This document is the single canonical place to see the complete DONE / TODO / REJECTED / DESIRABLE
state of the content-addressed (CAS) MergeTree feature across all areas. Items link back to the
section documents for technical detail.

Status stamps: **DONE** = implemented and validated; **PARTIAL** = core implemented, gaps remain;
**TODO** = agreed as necessary, not yet implemented; **DESIRABLE** = identified as valuable but
not committed; **REJECTED** = investigated and deliberately not pursued (reason recorded).

> **⚠️ Live pending backlog moved (2026-07-13 grooming).** This roadmap is now the **DONE / history
> status roll-up**; the single canonical list of *still-pending* work is [`BACKLOG.md`](BACKLOG.md)
> (IDs preserved, deduplicated, verified against HEAD). Since this file was last groomed (2026-07-11) a
> large amount landed and is NOT re-listed here as pending: the **ref snapshot+log migration** (the
> mutable `RootShardManifest` / per-`(ns,shard)` root-shard model is GONE), **mixed-algo pools**,
> **pluggable blob hash incl. sha256 Phase 2**, **part-folder cache**, **S3-native staging** (opt-in),
> **GCS generation binding**, **retired-in-snapshot**, **add-only GC freshness meta**, **writer↔GC
> simplification / freshness-v3**, the **introspection package**, and the **whole 2026-07-12
> stabilization iteration** (incl. B31 capability gate — now DONE). **Now-obsolete rows below** (kept
> for history, marked `OBSOLETE`): the root-shard-axis items — per-namespace `root_shards`, adaptive
> shard splits, the shard-mutation queue, B111, B158 — are moot under the per-table snapshot+log model.
> The 2026-07-13 **rev.6 lease-boundary exclusivity** proposal removes the grace-window / late-predecessor
> machinery (incl. the `CasRefLatePredecessorObserved` counter). See `BACKLOG.md` for all of it.

---

## Architecture and object model {#area-architecture}

See [`01-architecture.md`](01-architecture.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| Content-addressed blob storage (hash-keyed, dedup across replicas) | **DONE** | Foundation of the feature |
| Incarnation-token identity (S3 ETag / conditional-op token per blob per owner) | **DONE** | Replaced EBR GC core; enables exact-token deletes |
| Per-`(ns, shard)` ref shards (root shards replacing the namespace registry) | **DONE** (D1) | Registry removal landed; `shard incarnation` + `cas/refs/` layout |
| Immutable part-manifests (`cas/manifests/`) separate from mutable root shards | **DONE** | Hot/cold split (D0) |
| Pool layout: `blobs/`, `cas/refs/`, `cas/manifests/`, `roots/` (GC mutable), `gc/` | **DONE** | Format-frozen |
| Schema-evolution framework (`FormatId` self-describing envelopes) | **DONE** | `specs/2026-06-24-cas-schema-evolution-framework-design.md` |
| Zero-copy replication **coexistence** (opt-in per disk; not removed) | **DONE** | `metadata_type = content_addressed` is opt-in; zero-copy-replicated tables keep working |
| Merkle `treeId` tree layer | **REJECTED** | Removed at the core-refactor step; added complexity without correctness benefit; tree objects are now manifest-internal |
| EBR (Epoch-Based Reclamation) GC core | **REJECTED** | Replaced by incarnation-token GC; EBR required per-object epoch state that did not scale |
| Integer in-degree refcount | **REJECTED** | Replaced by the source-edge set; integer counts suffered from non-atomic fold + undercount under concurrent leaders |
| Namespace registry (`_registry` flat file) | **REJECTED** (removed by D1) | Monotone growth under repeated create/drop; replaced by per-shard incarnation allowing namespace-level reclaim |

---

## Writer protocol {#area-writer}

See [`03-writer-protocol.md`](03-writer-protocol.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| Build → precommit → upload → promote write path | **DONE** | |
| File-cache disk over a CA disk (2026-07-03) | **DONE (2026-07-08)** | Was: `<type>cache</type>` over a CA disk threw `NOT_IMPLEMENTED` at startup because `wrapWithCache` fronted the CA disk with the generic `MetadataStorageFromCacheObjectStorage` passthrough (hides `isContentAddressed` + the concrete CA metadata/transaction types the read/write paths `dynamic_cast` to). Fix (`3ed0e5f5030`): for a CA disk, reuse the CA metadata storage DIRECTLY under the cache disk and wrap only the object storage — safe because CA `startup`/`shutdown` are idempotent (base + cache disk share one mount/lease). Immutable content-hash blobs cache read-through; control plane bypasses the cache by construction. Design: `docs/superpowers/specs/2026-07-08-cas-file-cache-disk-support-design.md`; test: `tests/integration/test_cas_file_cache` (RED→GREEN, cache-hit metrics asserted); needed a new RustFS integration backend (`with_rustfs`) since MinIO doesn't enforce CA's conditional-PUT semantics |
| Excise the retired `Tree` object kind from the code (2026-07-03) | **DONE (2026-07-03)** | Renamed everywhere: `ObjectKind::Tree` removed from the enum entirely (`Blob` is the sole survivor); `CasEventType::TreePut/TreeExpand/TreeRetire/TreeDelete/TreeStrip` → `ManifestPut/ManifestExpand/ManifestRetire/ManifestDelete/ManifestStrip` (`toString` values `tree_*` → `manifest_*`); `CasEventObjectKind::Tree` → `::Manifest` (`toString` `"tree"` → `"manifest"`); the 11 `CasTree*` ProfileEvents → `CasManifest*`; `CasNs::Tree` → `CasNs::Manifest` and the legacy `/trees/` classifier branch deleted (the `/cas/manifests/` branch is the only survivor); the `CATR` magic + envelope/codec Tree arms and the `objectKey(..., ObjectKind::Tree, ...)` LOGICAL_ERROR thrower removed; Tree-exercising unit tests deleted (mixed-kind sort/round-trip cases collapsed to Blob-only, since `ObjectKind` has one surviving value) |
| Staging area inside the bucket (2026-07-03) | **TODO (needs brainstorm/spec)** | Support a dedicated staging prefix in the pool bucket: objects uploaded out-of-band (bulk load, backup restore, external tooling) land under staging and are ADOPTED into the pool via the verified copy-forward/adopt path (hash-verify, then publish) instead of being trusted in place; scope and exact semantics to be specced |
| Durable-monotone `writer_epoch` / `build_sequence` identity | **DONE** | Prevents stale-epoch cross-publish |
| Mutable vs immutable file classification | **DONE** | `.bin`, mark files, `primary.idx` → blobs; others → verbatim or inline |
| Streaming `Build::putBlob` (no whole-blob memory materialization) | **DONE** | Was materializing `BlobSource` into a `String`; now streams from staged temp file; peak memory ~2x instead of ~6.5x |
| Manifest soft/hard limits with backpressure (B164b) | **DONE** | `manifest_soft_limit` paces writes; `manifest_hard_limit` throws `LIMIT_EXCEEDED` before any ref is published |
| Precommit-first publish (durable precommit before blob upload) | **DONE** | Ensures abandoned precommits are visible to GC |
| Dedup cache (known-present blob cache) | **DONE** | Configurable `dedup_cache_bytes`; a hint only, never affects correctness |
| Adaptive HEAD-before-PUT (skip body upload for known-present blobs) | **DONE** | `dedup_head_first_min_bytes` threshold |
| Replication fetch-by-relink (same-pool part relink, zero byte cost) | **DONE** | `test_cas_replicated_relink` integration test passes |
| `manifest_hash` field on Keeper `/parts` znode (cross-replica header divergence detection) | **REJECTED (2026-07-14)** | B1: replication stays disk-agnostic — no CA-specific field in the Keeper part header (fork-surface + replication-complexity cost; fetch-by-relink gets the manifest id in-band). See `BACKLOG.md §obsolete`, `01-architecture.md §benign-cross-replica-divergence` |
| Streaming `putOverwrite` path (condemned-displacement case) | **DESIRABLE** | The rare INV-1 revival/displacement path still materializes whole body; not a blocker |
| CA INSERT peak-memory overhead vs local (~9 MiB) | **CHARACTERIZED (2026-07-10)** | Memory-profiled (`trace_type='Memory'`) on a 10 k × 10 KB FixedString INSERT: peak CA 144.5 MiB vs local 135.5 MiB; the ~95 MiB bulk is the column block (identical to local), the ~9 MiB delta is `ContentAddressedTransaction::writeFile` staging/hash buffering (`putBlob` already streams — see above — so this is a fixed write-buffer cost, not whole-file materialization). Drove test `03829` `max_memory_usage` 150M→170M. Shaving it further (stream-hash the staging buffer) is minor/optional |
| `clickhouse local` shutdown hang (GC thread not reaped on local exit) | **TODO** | B48: background `BackgroundSchedulePool` / GC thread not joined on `LocalServer` teardown |
| Relink-into-detached fetch (zero-byte `to_detached` fetch for same-pool parts) | **TODO** | B66b: currently byte-streams; extend `Fetcher::relinkPartToDisk` to honor `to_detached` |
| Concurrent-fetch torn read of shared `detached` ref on local storage | **TODO** | B66a: `LocalObjectStorage` write is not atomic; safe on S3 (atomic PUT) |

---

## GC protocol {#area-gc}

See [`04-gc-protocol.md`](04-gc-protocol.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| **Deposed-leader stray-Clean `clearSparedMeta` → live-blob delete** | **DONE (2026-07-11)** — fixed via ADD-ONLY GC freshness meta (remove spare-side `clearSparedMeta`); code `730b59cd686`, TLA `96c571700382` (`post_adoption_clear` sabotage RED proves Fix 1 insufficient). See [`specs/2026-07-11-cas-deposed-leader-clearsparedmeta-fix-design.md`](../specs/2026-07-11-cas-deposed-leader-clearsparedmeta-fix-design.md). Original finding: | A deposed leader's pre-CAS `clearSparedMeta` (spare verdict, completed before the `gc/state` CAS, `CasGc.cpp:106-113`/`:415`/`:513`) leaves a durable stray-**Clean** meta over a still-`delete_pending` blob → a writer dedup-reuses the condemned exact token → the pending exact-token redelete deletes the live reuse (INV_NO_LOSS). Pre-existing in v3; low-prob (concurrent leaders + reuse race), high-impact (data loss). TLA witness `CaRetiredInRunFoldAbortWitness` reproduces it (RED); add-only advisory meta is green. Fix candidates + full trace: [`reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md`](../reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md). Needs a brainstorm + its own gate |
| GC-protocol narrative refresh (`04`/`05`/`07`) for the accumulated v3 + retired-in-snapshot deltas | **TODO (doc-debt, 2026-07-11)** | The GC-protocol prose in `04-gc-protocol.md`, `05-formats-and-backend.md`, and `07-s3-budget.md` still describes the pre-refactor world: the **three-cursor** merge (now two-cursor), the separate **retired-list run** / `retired_refs` publish (now `kCondemned` rows in the snapshot run + seal `condemned_summary`), and **ack-floor `min_ack` graduation** (now round-paced, writer reads per-hash `.meta`). Code + tests are correct and green; only the narrative lags. A careful section-by-section rewrite reconciling v3 freshness-meta + retired-in-snapshot is needed. `05 §object-key-tree` (`retired/<round>/<shard>`), `05 §magic-table` (`CART`), and the `07` S3-budget rows (prior-retired-run GET, retired-run PUT) are the concrete stale anchors. |
| GC leader election, lease, and advisory heartbeat | **DONE** | Lease prevents concurrent leaders; heartbeat (B160) reduces stale-lease wait |
| One-pass ack-floor round (heartbeat floor → three-cursor merge → single `gc/state` CAS) | **DONE** | On `cas-gc-ack-floor-fence`; replaces fence+recheck; O(delta)+O(servers) per round; soak validation TODO. See [`04-gc-protocol.md §gc-round`](04-gc-protocol.md) |
| Merged heartbeat (mount lease ∪ build watermark ∪ GC ack in one beat) | **DONE** | `WatermarkKeeper` + `CAWM` object removed; `MountLease` carries `min_active`, `observed_gc_round`, `gc_fenced`; −1 PUT/beat |
| Two-phase graduation (`delete_pending`) | **DONE** | Zombie-safe: pre-CAS deletes only for previously-published pendings; deletion lags condemn by one pass |
| Ack-floor TLA+ gate (`CaGcAckFloorCore` + `CaGcAckFloorZombie`) | **DONE** | 7 sabotages + 3 witnesses; `delete_pending` proved load-bearing; floor-before-cut order invariant. See [`06-tla-models.md §area-ackfloor`](06-tla-models.md) |
| Per-round all-shard fence + fold-through-fence recheck | **REJECTED** (replaced by ack-floor) | Were ~2×O(universe) GET + O(universe) CAS-PUT per round; the causal ack floor gives the same create-ordering + spare guarantees without per-shard fence writes |
| Attempt-scoped generations (deposed leader's artifacts under unadopted attempt) | **DONE** | Fixes GC-CONCURRENT-LEADER-LEAK; every per-round artifact keyed by `(gen, attempt = lease.seq)` |
| Source-edge in-degree set (replaced integer count) | **DONE** | Eliminates undercount under concurrent leaders; `(blob_hash, source_id)` edge per manifest entry |
| Snap prune (retention prune of old GC generation artifacts) | **DONE** | Prevents unbounded `gc/gen/*/` growth |
| D1: shard-object reclaim + shard incarnation (namespace reclaim without a registry) | **DONE** | Prevents monotone GC fanout from repeated create/drop |
| Orphan part-manifest sweep (reclaim unreachable `cas/manifests/` objects) | **DONE** | `CasOrphanManifestSweep`; uses build watermark |
| GC-CONCURRENT-LEADER-LEAK (reclaim liveness bug) | **DONE** (see REJECTED / FIXED above) | Root cause: non-atomic fold-seal + `gc/state` CAS; fixed by attempt-scoped generations |
| Ack-floor round soak validation | **PARTIAL** | 2026-07-03 night: kill-chaos soaks exercised the floor end-to-end and FOUND+FIXED the clamp-graduation hole (see the clamp-suppression row); still-unrun named checks: SIGSTOP writer holds-then-releases the floor; O(delta)+O(servers) request-count regression guard (proposed scenario cards exist in `scenarios/BACKLOG.md`) |
| Self-remount on GC fence-out (a fenced live server re-opens a FRESH incarnation via the S13 mount machinery: epoch bump + immediate `gc_fenced` reclaim + fresh view + build invalidation) | **DONE** | Found by the 2026-07-02 ack-floor soak: a 51s CH pause + concurrent GC round fenced the mount; the keeper fails closed by design (sleeper re-arm is the TLA+ sabotage), liveness needs the fresh-incarnation path. Interim: ca-soak chaos caps CH pauses at 20s. Also soften the `Store` teardown release message for the `gc_fenced` case (a foreign incarnation there is the EXPECTED fence-out outcome, not corruption) |
| Ack-floor observability (Task 11) | **DONE** | `CasGcRetired{Condemned,Spared,Graduated,Redeleted}` + `CasGcHeartbeatFenceOuts` + `CasGcFloorHeldByStaleAck` ProfileEvents; `gc_fence_out` audit event; WARNING when a live heartbeat's ack lags the round by > 2; `RoundReport` carries condemned/graduated/redeleted/fence_outs/min_ack |
| `mayMutate` fence deadline on `CLOCK_BOOTTIME` (Task 12) | **DONE** | `steady_clock` does not advance across a VM suspend; the write-fence deadline is now a `CLOCK_BOOTTIME`-ms instant (`Store::bootMs`, injectable via `PoolConfig::boot_ms_fn`) so a resumed VM sees its fence expired (container pause already safe) |
| Snapshot streaming reads (memory O(block); true ranged `get` + `getStream` seam + streaming `RunFileReader`) | **DONE** (T2) | 2026-07-02; opening a run is 3 requests (`head` + tail `get` + body `getStream`), 4 for a large footer; `seek` = +1 ranged `get`/block; the whole-run `full` member is gone. See [`04-gc-protocol.md §snapshot-run-reads`](04-gc-protocol.md), `specs/2026-07-02-cas-gc-snapshot-streaming-design.md` |
| Reference-parent runs for empty-delta gc-shards (idle rounds touch zero run objects) | **DONE** (T0) | 2026-07-02; an empty-delta + empty-retired shard is pure ref-carry (zero run I/O); `RunRef` gains `shard` + `generation`; consumers resolve runs via seal refs; ref-aware retention + post-CAS hand-off delete. See [`04-gc-protocol.md §ref-aware-retention`](04-gc-protocol.md) |
| **Retired-in-snapshot: fold the retired list INTO the snapshot run (3-cursor → 2-cursor merge)** | **DONE (2026-07-11)** | `specs/2026-07-10-cas-retired-in-snapshot-design.md` + `plans/2026-07-10-cas-retired-in-snapshot.md` (T1–T8). The separate per-gc-shard `RetiredSet` object family is gone: condemned state now rides the source-edge run as `kCondemned` sentinel rows at the zero-sentinel key, and the fold seal carries a per-gc-shard `condemned_summary` (`condemned_total`, `pending_total`, `oldest_nonpending_condemn_round`, TOTAL over `gc_shards`). The merge is 2-cursor (prior run + deltas); `graduationDue` reads the seal summary zero-I/O; rebuild folds orphan condemns into the single flush; consumers (dryrun/fsck/ca-inspect) read the runs/summary. Deleted: `RetiredSet`/`encodeRetiredSet`/`decodeRetiredSet`, `retiredKey`, `GcState::retired_refs` (proto field `reserved`), `FormatId::RetiredSet`/magic `CART` (freed, never reuse). TLA gate `CaRetiredInRun` green + 3 sabotage red. **Doc debt:** the `04`/`05`/`07` GC-protocol narrative still describes the old 3-cursor / ack-floor `min_ack` prose — see the "GC-protocol narrative refresh" TODO below. |
| Delta-runs + compaction for the snapshot (bytes O(edges)/pass) | **DESIRABLE** (T1) | The HOT-pool full snapshot rewrite per pass is the next dominant byte cost; builds on the T2/T0 primitives (streaming reader with `seek`, `getStream`, ranged `get`, seal-ref resolution) unchanged. NEXT spec; source `specs/2026-07-02-cas-gc-snapshot-streaming-design.md §what-deliberately-does-not-change` |
| GC round progress observability (round-duration watchdog, LIST/window progress events, alert on `gc_fold_begin` without `gc_fold_end`) | **TODO** | Motivated by the 2026-07-02 soak forensics: a long/wedged round is currently only visible after the fact; emit a round-duration watchdog + LIST/fold-window progress events + an alert on an unbalanced `gc_fold_begin`/`gc_fold_end` pair |
| `process_epoch` → `writer_epoch` stamp unification | **DESIRABLE** | The writable path already sets `process_epoch = writer_epoch`; unify the manifest `writer_instance_id` stamps |
| Clamp-suppressed GC passes (no graduation / no pending deletes while any shard is clamped) | **DONE** | 2026-07-03 night SAFETY fix (`c47d10d01ec`): clamps break the ack-floor lemma 'landed before the cut => folded before graduation' (the model's SabotageSkipChangedShard, realized — 31 dangling in the night soak, caused by RustFS false 404s under the #3231 storm); a clamped pass carries everything, deletes resume on the first clamp-free pass; `gc_fold_clamp` event per clamp. See `04-gc-protocol.md §absent-at-head` |
| TLA+ model extension: clamps + destruction suppression | **DONE** | 2026-07-03: honest clamp in `GFold` (fold may hold back one landed ref but DECLARES it via `clampedL` — vs the still-lethal undeclared `SabotageSkipChangedShard`), suppression guard in `GComplete` grads, `SabotageClampNoSuppress` reproduces the night's INV_NO_DANGLE counterexample, `W_ClampHappens` witness fires; honest stage-1 CLEAN (83.9M distinct states); all 10 prior sabotages + 6 witnesses re-verified |
| B207 fsck consistency race (phantom dangling under concurrent GC) | **RESOLVED 2026-07-11** (`94970514116`) | Restored from the pre-consolidation backlog (lost in the fold): `runFsck`'s ref-walk and HEAD-confirm are minutes apart with no snapshot — a re-published ref + a legitimate GC delete manufactures a false `dangling`. FIX: at the HEAD-absent branch, RE-RESOLVE the referencing ref(s) (labels already collected); only a CURRENT ref over an absent object is dangling. Gates honest release-validation soaks (B185/B206/B144 were all this race) |
| Verified copy-forward for condemned tokenless-evidence deps at the promote gate | **DONE** | 2026-07-02, `specs/2026-07-02-cas-copy-forward-condemned-evidence.md`: fixes the S13 soak-run-3 attach brick (`republishRef` -> promote `ABORTED` -> table readonly forever). Narrow INV-1 exception (committed-source evidence only; full content verification; token-conditional `putOverwrite`); TLA+ `WCopyForward` gate; `blob_copy_forward` event + `CasBlobCopyForward` counter. See [`03-writer-protocol.md`](03-writer-protocol.md) |
| Promote-time in-place recreate of a condemned SOURCED blob | **DESIRABLE** | For tokened (sourced) deps the promote gate stays fail-closed `ABORTED` (build-local sources not retained at promote); recreate happens on the retried build via `putBlob` cold-reuse. The tokenless-evidence case is DONE (copy-forward above) |
| fsck pipeline classification (`pending-gc` / `awaiting-gc` / `unaccounted` replace the suspicious `unreachable` lump for blobs; de-alarm notes) | **DONE** | 2026-07-03, from the raw-audit RFC triage: deletion lag of the two-phase pipeline is now labeled as the expected state it is; `unaccounted` (outside the whole GC view) is the anomaly signal (INV-2). See `08-testing-and-soak.md §fsck` |
| Raw GC rebuild (`gc/state` disaster recovery) | **DONE** | 2026-07-03, `specs/2026-07-03-cas-gc-rebuild-design.md`: the "план Б" survivor of the 2026-06-30 raw-audit RFC. A fail-closed baseline guard (`CORRUPTED_DATA` when a shard journal proves trimmed history with no healthy adopted seal) ships ahead of `Gc::rebuildBaseline(force)` — derived-bookkeeping only, over-protect only (synthetic baseline from owner replay + EMPTY retired lists + round minted above every surviving mount ack/fence-round/generation), single `gc/state` CAS. Surfaced as `SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] [<disk>]` and `clickhouse-disks ca-gc-rebuild [--force]` (read-only-open required). Registry-repair/orphan-sweep/debris-prune parts of that RFC are obsolete (registry removed by D1; sweeps live in regular rounds; no structural orphan-blob class per INV-2). See `04-gc-protocol.md §gc-rebuild`, `08-testing-and-soak.md §gc-rebuild-runbook` |
| B94 full-GC/check backstop for physical debris/drift | **DONE** (by composition) | `clickhouse-disks fsck` (pipeline-classified) + `ca-gc-dryrun` + the raw GC rebuild cover the audit/backstop surface; regular GC stays incremental by design (raw-audit RFC non-goal) |
| RustFS false-404-under-load upstream report (HEAD returns 404 for live objects while the metacache is degraded by rustfs#3231 dir bloat; caused the 2026-07-03 clamp era) | **TODO** | Build a repro on top of the rustfs#3231 repro (bloated dirs + concurrent stat); our side is already safe (clamp + destruction suppression, `04-gc-protocol.md §absent-at-head`) |
| Per-namespace `root_shards` (chosen at table creation) | **OBSOLETE (2026-07-13)** — root shards removed by the per-table snapshot+log; no `shardOf` consumer remains | 2026-07-03 weighing: GC needs no N (`discoverUniverse` LISTs, the fold digests what exists) — only the owning writer's/readers' `shardOf` does, so a per-namespace meta object (putIfAbsent at first publish, immutable) + a DDL hint (`SETTINGS cas_root_shards=N`) suffices. Payoff is at the 100k-table scale: discovery keys ∝ Σ N over all tables — cold tables at N=1-4 collapse it while hot tables keep 32-64. No resharding: a wrong guess lives like today's pool constant, per-table |
| Adaptive shard SPLITS (hash-prefix radix, writer-local) | **OBSOLETE (2026-07-13)** — ref sharding removed by the per-table snapshot+log | Avoids the killer (mass cross-shard renames = two-owner/zero-owner windows): N is a power of two, shard = top hash bits; splitting hot shard k into k0/k1 partitions refs deterministically by the next bit — a LOCAL single-writer op. Load-bearing precondition: split only when the shard is FULLY FOLDED AND TRIMMED (cursor == shard_version, no clamps, no live precommits) so children start with empty journals; incarnation stamps already handle new-object-at-path (ABA); GC edges unaffected (source_id is shard-independent). Reader routing = radix over shard object names ("0","10","11") with a self-healing re-LIST cache. The writer feels its own heat (flush latency = body size) and splits itself; merge-back for cold shards by the same shape. Real cost: spec + TLA+ for split x fold/trim/precommit interleavings + fsck awareness. Main prize: the cold tail of large installations + insurance for wrong per-table hints |
| Sweep dangling `M-W`/plan references from code comments | **TODO** | ~15 comments reference the deleted `plans/2026-06-12-ca-core-m-w.md` ("M-W T3", "D-W1", "M-W design section 4") — replace with self-contained wording or `docs/superpowers/cas/` pointers |
| Shard-mutation flat-combining queue (group commit per ref shard) | **OBSOLETE (2026-07-13)** — the root-shard mutation path it optimized was replaced by the `CasSingleWriterSlot` per-table ref-append lane (snapshot+log); its `CasShard*` ProfileEvents were removed in the 2026-07-12 stabilization. Was: **DONE** 2026-07-03, `specs/2026-07-03-cas-shard-mutation-queue.md`: soak measured 637k casPut attempts for 380k landed (40% conflicts, 92% under storms, each conflict re-reading ~280 KB) from up to 156 concurrent mutating threads over 64 shard keys with NO intra-server serialization; the queue makes intra-server conflicts structurally impossible and compresses bursts into single casPuts (fewer rustfs#3231 leaks until upstream fixes). See `03-writer-protocol.md §shard-mutation-queue` |
| Writer/mount introspection insights | **DONE** | 2026-07-02: `retired_view_advance` event on every view ADVANCE (installed round, prior round, retired-entry count loaded), `mount_remount` event (ok/failed) from the self-remount loop, and the GC round log gained the ack-floor pipeline columns (`entries_condemned/graduated/redeleted`, `fence_outs`, `min_ack`, `anomalies` — replacing the dead always-0 cascade/forget columns) |
| GC discovery O(N²) LIST quadratic over `roots/` | **DONE** | 2026-06-29..07-01, superseded by two landed fixes: (1) discovery LISTs `cas/refs/` (a flat one-object-per-`(ns,shard)` prefix), NOT the manifest-heavy `roots/` tree — `f5f96dce01a` (relocate) + `644eb7c6ade` (D1: `discoverUniverse`/`listRootShardTokens` = `LIST(cas/refs/)`, registry deleted); (2) `ObjectStorageBackend::list` is real streamed pagination via `object_storage->iterate(prefix, start_after=cursor)` — no more `listObjects(max_keys=0)` re-enumerate-per-page on the native path — `b15f1ef9d28`. `CasGc.cpp` has NO `rootsPrefix()` in discovery (verified 2026-07-06). Release gate #16 is closed. NOTE: this is the DISCOVERY-PLACEMENT quadratic only; the remaining O(universe)-per-round cost (two full `cas/refs/` LISTs + generation re-read every round regardless of delta) is the separate **fold/discover skip-unchanged** item below |
| GC round is O(universe) not O(delta) — fold/discover skip-unchanged | **DONE** (Phase 4 Lever A), `436714d80f0`..`3cba4f812f8` | Spec `specs/2026-07-06-cas-gc-round-skip-unchanged-design.md`, plan `plans/2026-07-06-cas-gc-round-skip-unchanged.md`. A round making no destructive decision re-adopts the sealed in-degree generation (DEFER) instead of rebuilding it (~2×O(universe) snapshot read+write, ~1362 CasGcGet/idle round). Decision from cheap pre-fold signals (changed-shard count; graduation-due). Safety invariant: no destructive decision on a not-fully-folded snapshot ⇒ due graduation force-folds (mirror of the 2026-06-27 leak). Mandatory TLA+ gate `CaGcRoundDeferCore` (NoOverDelete + EventuallyFolded), soak harness ops-budget assertion in `S03`. Lever A DEFER short-circuit landed + TLA+-gated; Lever B (incremental point-updatable in-degree — makes even a non-idle small-delta round O(delta)) still open |
| Adaptive GC cadence — journal-pressure-triggered fold | **DESIRABLE** (follow-up to Phase 4 Lever A) | The real cost of a rare GC is NOT S3 storage of dead data (nearly free short-term) but **journal growth**: every writer mutation RMW-rewrites the whole root-shard body (live-refs + journal tail); the tail is trimmed only up to the fold cursor (GC-driven), so without folding the per-mutation CAS cost grows ∝ journal tail ∝ rate×(time-since-fold) — quadratic in mutations without trim. Total S3 ops/sec ≈ A/interval (GC O(universe)/fold) + B·interval (writer amplification) ⇒ sqrt-optimal interval ∝ √U/r, HARD-capped by the hottest shard's body staying < object-store inline threshold (~128 KiB RustFS; 8 MiB soft-limit backstop) — night data: 32 shards, hot table ~25 KB healthy / ~165 KB @ 10 min under a storm. KEY: the fold trigger should be **journal-pressure per shard (size/age)**, NOT changed-shard count — one hot shard bounds deferral regardless of how few shards changed. Prod direction: modest `gc_interval_sec` (~30–60 s, not the test-only every-few-seconds; idle DEFER makes idle pools ~free) + journal-pressure fold trigger; constants pool-specific ⇒ measure (soak sweep: hot-shard CAS body size vs GC ops/sec, find the knee). See BACKLOG `ADAPTIVE-GC-CADENCE` |
| Common-shard-prefix for GC discovery (IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST) | **DONE / realized** | Shard (ref) objects already live in ONE flat prefix `cas/refs/<ns>/<shard>` since the Phase-1 relocation (`f5f96dce01a`), so GC discovery IS a single paged `LIST(cas/refs/)`. The idea's goal is met; nothing further to relocate |
| Run-file O(buffer) streaming | **DONE** (T2, 2026-07-02) | `RunFileReader` streaming mode (borrowed-memory + streaming); true ranged reads in `CasObjectStorageBackend` + `getStream` seam; the whole-run `full` member is gone. See the T2 row under [GC protocol](#area-gc) |
| `inDegreeInGeneration` O(candidates × runsize) | **RESOLVED** by the ack-floor round | The per-candidate recheck whole-run re-read is gone — the retired cursor rides the single three-cursor merge; the function remains only for preview/tests |
| `SYSTEM CONTENT ADDRESSED GC RUN [<disk>]` command | **DONE** | Synchronous explicit GC trigger; logs to `system.content_addressed_garbage_collection_log` |
| GC S3 budget: HEAD storm (B148) — `resolveRef` HEAD per warm hit + condemn HEAD per **new** candidate | **PARTIAL** | The retire/recheck O(universe) HEAD+GET phases are gone (ack-floor round); the remaining condemn HEAD is bounded by newly-condemned candidates, and the discovery LIST is now the dominant scale item (see the O(N²)-LIST row) |
| B168 op-count reduction program (P4/P7: fewer metadata writes) | **PARTIAL** | Some items done; the fence-related items (P6 dirty-only fence) are moot — the fence is gone (ack-floor round). Remainder tracked in `07-s3-budget.md §reduction-history` |

---

## Formats and backend {#area-formats}

See [`05-formats-and-backend.md`](05-formats-and-backend.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| One-header envelope format with `FormatId` self-description | **DONE** | All object kinds use a common framing |
| Protobuf codecs for root shards and part-manifests | **DONE** | Replaced earlier ad-hoc serialization |
| `putDeterministicArtifact` (byte-equal-or-`CORRUPTED_DATA` for sealed per-round artifacts) | **DONE** | Enables safe resume of incomplete GC rounds |
| Exact-token deletes (`deleteExact` — the only reachability delete in the core) | **DONE** | INV-NO-LOSS guarantee; stale token → spared, never over-deleted |
| `Cas::Backend` abstraction over `IObjectStorage` | **DONE** | Enables testing over `LocalObjectStorage`; composes with any object-storage type |
| Schema-evolution stance: no compat scaffolding during pre-release dev | **DONE** | Spec `2026-06-24-cas-schema-evolution-framework-design.md`; first persisted-data release will freeze the format |
| rustfs testbed (scanner/heal off for stability) | **DONE** | Production deployment needs a compacting object store |
| Real-S3 GC validation (confirm GC deletes actually reclaim on production AWS/GCS/Azure) | **AWS + GCS DONE 2026-07-03**; Azure TODO (HARD release gate) | AWS: probe/replication/dedup/two-phase reclaim/DROP-to-zero all verified live (see §release-required #1); rustfs beta.8 does not compact tombstones |
| LIST consistency on real S3 (token-diff accelerator behavior under eventual consistency) | **TODO** | S3's LIST may not reflect a just-PUT key; the code handles this conservatively but needs real-S3 testing |
| `B196` cap `s3_max_connections` to backend permits | **TODO** (HARD, cheap) | Prevents 503 + retry storm under high concurrency |

---

## S3 op budget {#area-s3-budget}

See [`07-s3-budget.md`](07-s3-budget.md) for the full breakdown.

| Item | Status | Notes |
|------|--------|-------|
| Dedup cache (known-present blob → skip HEAD + PUT body) | **DONE** | |
| Adaptive HEAD-before-PUT | **DONE** | |
| Precommit-first (write precommit before blob upload; reduces orphan debris) | **DONE** | |
| Snap prune (remove old GC gen artifacts → fewer GC GETs per round) | **DONE** | |
| LIST-token skip (skip unchanged root shards using `ETag` token diff) | **DONE** | |
| Streaming `Build::putBlob` (eliminates duplicate memory copies during upload) | **DONE** | |
| HEAD storm at retire (per-candidate HEAD in retire, not stored token) | **TODO** | B148; dominant cost at scale; stored-token optimization deferred (requires manifest schema change) |
| Root-shard fan-out vs per-object permit cap (B158) | **DONE** (superseded) | The flat-combining shard-mutation queue removed intra-server CAS contention structurally; `root_shards` default weighed to 32 (2026-07-03) for body-size/batching/discovery balance, not contention |
| `RENAME` = one Build/part (B111) | **DESIRABLE** | Currently multiple root-shard updates per rename |
| Ack-floor round (removes the O(universe) fence + recheck phases) | **DONE** | The per-round all-shard fence is gone; dirty-only-fence (P6) is superseded. See `07-s3-budget.md §gc-budget` |
| Read-only fsck shadow disk breaks table load on restart (2026-07-03, GCS stand) | **TODO (prod gate — the fsck pattern we recommend)** | A `ca_ro` read-only disk over the SAME pool + same `server_root_id` makes MergeTree part discovery find every part twice: table load fails with `UNKNOWN_DISK` ("Part ... was found on disk 'ca_ro' which is not defined in the storage policy") on any server restart with CA tables attached. Sub-project A's fsck pattern hits every user. Stand workaround: keep the ro disk OUT of the server config, in a standalone file passed only to `clickhouse-disks -C` (see `utils/ca-soak/configs/fsck_only_gcs.xml`). Product fix candidates: part discovery skips `readonly` object-storage disks, or a `hidden`/`introspection_only` disk flag; needs a small design pass. **Triage answered 2026-07-06:** the RustFS default stand DOES hit it — confirmed on a FRESH simple `MergeTree` table (1000 rows, no merge) + a GRACEFUL restart, `UNKNOWN_DISK` on `ca_ro`. It was simply never re-run after `ca_ro` was embedded in the RustFS server config (`cbe0ffb7608`, 2026-07-03 02:04); prior passing S13 runs predate that. The GCS stand workaround (standalone `clickhouse-disks -C` config) is now propagated to the default stand: `ca_ro` removed from `storage_conf_ch1/ch2.xml`, moved to `configs/fsck_only_ca.xml` mounted at `/etc/clickhouse-server/fsck-only.xml` (outside `config.d`), `soak/fsck.py` points there. Product fix still OPEN (this is only the stand workaround; `10replicas`/`gc_shards2`/`awss3` server configs still embed `ca_ro` and need the same treatment before their restart scenarios run) |
| CREATE/load empty-namespace HEAD storm (2026-07-03, operator stand) | **DONE 2026-07-03** (`3d278522e18` + guard `c6b50d73503`) | `Store::listRefs` HEADs ALL `root_shards` (32) per `existsDirectory`/`listDirectory` on an empty namespace, absence is never cached (`readShardDecoded` erases on 404) — was: a fresh `CREATE TABLE` cost 3 sweeps = ~102 HEAD-404s (20 s at 175 ms/op). FIX: `Store::listRefs` now LISTs `cas/refs/<ns>/` once and decodes only PRESENT shards — live re-measure on the GCS stand: `CasRootHeadMiss` 100 -> **1** per CREATE. The (b) negative-cache half was deliberately NOT built (YAGNI — one LIST per exists call suffices); revisit only if numbers demand. Classifier fixed same day |
| Capacity model: GC cadence + snapshot size under typical load (2026-07-03) | **TODO** | Estimate, from measured per-insert/merge event volume, how often GC must run and how large the per-shard in-degree runs / fold seals / retired lists get at a typical production load (inserts/s, parts/day, root_shards=32); validate the estimate against a soak's `system.content_addressed_garbage_collection_log`. Feeds the `gc_interval_sec` default and the trim gates; live-AWS data point: a round takes 30-40 s |

---

## Read protocol {#area-read-protocol}

See [`09-read-protocol.md`](09-read-protocol.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| `resolveRef` via root-shard decode + TTL/single-flight caches | **DONE** | `CasStore.cpp:530`; shard decode cache at `CasStore.cpp:392` |
| `(ManifestId, Token)` manifest decode cache | **DONE** | `CasStore.cpp:576`; token-keyed; wholesale-clear bounded |
| Blob ranged GET (`getBlobViewPlan` + `readBlobPayload`) | **DONE** | One ranged S3 GET per column file per part open |
| `ReadBufferFromFileView` position-rebase fix (B115) | **DONE** | Commit `440871098a9`; gtest added; latent in `PackedFilesReader` statistics path |
| Column pruning (structural — per-file `lookupPath`) | **DONE** | Reader requests only needed files; CA layer has no filter list |
| Inline / mutable / verbatim file reads (0 extra S3 ops) | **DONE** | `tryGetInManifestBytes`, `prepareInManifestRead` |
| In-flight read-your-writes overlay (B59 — blob + directory) | **DONE** | `tryGetInFlightStorageObjects` / `hasInFlightDirectory`; projection workaround removed |
| B121 per-blob-GET read cost on large parts (one ranged GET per column file per part open) | **DESIRABLE** | Restored from the pre-consolidation backlog; relates to B202/one-GET-open below |
| B202 inline placement by SIZE only (+ threshold as a disk setting) | **DESIRABLE (design pass)** | Restored: drop the file-type predicate, inline everything < ~512 KiB; weigh the wide-part-medium-column selectivity regression (hybrid: keep a `.bin` carve-out). Pure perf/request-count tradeoff, no safety dimension |
| One-GET part open (pack small files; serve from memory) | **DESIRABLE** | Restored (was B10 #7): with B202 small parts open in ~1 GET |
| Cacheless-read cost (every read — incl. warm/repeat — round-trips to the object store; no local byte cache) | **CHARACTERIZED (2026-07-10)** | Profiled on the CA-s3 lane: raw CA warm read ~3× local (141 ms vs 47 ms; 186 S3 GETs; warm never improves). `trace_log` hot path = `ReadBufferFromS3::nextImpl` → socket receive/poll, NOT the CA metadata layer (prefetch on, marks cached, resolve cheap, ~1.2 MB/GET). Mitigation = the opt-in file-cache disk over CA (see §area-writer "File-cache disk"): warm ≈ local, 0 S3 GETs. **Guidance:** add a cache disk for re-read-heavy workloads; it does NOT help one-shot scans (cold-populate cost makes the first read ~2× slower). |
| `manifest_size` field in `Resolved` always 0 | **TODO** (minor, B10) | `resolveRef` never sets it; harmless but imprecise |
| Replication fetch-by-relink (zero byte cost for same-pool parts) | **DONE** | `manifest_hash` on Keeper `/parts` znode REJECTED (B1, 2026-07-14) — manifest id travels in-band |

---

## Operability and release readiness {#area-operability}

| Item | Status | Notes |
|------|--------|-------|
| `system.content_addressed_garbage_collection_log` | **DONE** | Per-round GC audit log |
| `system.content_addressed_log` | **DONE** | Per-event CA audit log; **on by default** since `cbe0ffb7608` (experimental feature → audit log is the primary forensic instrument; zero cost without a CA disk) |
| System logs on a CA-S3 disk (frequent small flushes) | **DONE (2026-07-10)** | The June B86 test workaround pinning system logs to the local `default` policy is REMOVED — CA-S3 log flushes are fast now (trace_log 7087 rows in 77 ms; no 180 s timeouts). Do NOT re-add B86. See §area-read-protocol "Cacheless-read cost" for the read-side companion |
| `clickhouse-disks fsck` | **DONE** | Independent pool reachability verification |
| `clickhouse-disks ca-gc-dryrun` | **DONE** | GC delete preview (zero writes) |
| Read-only disk mode (WORM deployment) | **DONE** | |
| `SYSTEM CONTENT ADDRESSED GC RUN` command | **DONE** | |
| Capability gate: reject unsupported ops at `CREATE`/`ATTACH` with clear error (B31) | **DONE (2026-07-13 verified)** | `supportZeroCopyReplication()==false` for CA (B31 comment, `DiskObjectStorage.h:54`); `supportsHardLinks()==true` deliberately (mutations route through a whole-part transaction); unsupported ops rejected by independent gates (ALTER PARTITION throws `not supported on a content_addressed disk`; BACKUP restore routes through a whole-part transaction) |
| `SYSTEM` control commands: START/STOP GC, POOL READONLY, CHECK (B197) | **TODO** (HARD) | |
| `system.*` views for pool/blob/part refcounts + GC status + frozen snapshots (B15/B99/B169/B159) | **PARTIAL** | GC log + event log + fsck/dryrun/rebuild CLI done; per-part/ref views and a `clickhouse-disks` decode/introspect (top-down traversal) surface not yet |
| Backup/restore runbook (B198) | **TODO** (HARD) | |
| Pool-format version breadcrumb (B180) | **TODO** | Self-describing pool meta for version identification |
| Integration tests on RustFS (not MinIO) (B125) | **TODO** (HARD) | Current integration tests use MinIO; production uses S3-compatible backends |
| Repo hygiene — non-shippable files removed from the diff (B131) | **TODO** (HARD) | Blocks a clean upstream PR |
| `CaWiringOps.FreezeViaHardLinksIntoShadow` gtest failure (B3 / B186) | **RESOLVED 2026-07-11** (`ecb6e1a5e58`) | intermediate-dir `existsDirectory` was raw-LIST-based (counted tombstoned-not-yet-GC'd objects); made tombstone-aware (`listNamespaces`+`listRefs`), GC-timing-independent. CA gtest battery fully green 669/669 |
| Migration path for existing tables (B13) | **TODO** | `ALTER TABLE … MOVE PARTITION` to a `content_addressed` disk re-packs; rollout safety spec needed |
| Server OOM at hour-4 soak (~49 GiB RSS, B165) | **TODO** (HARD) | Not reproduced since the `putBlob` streaming fix; re-run long soak to confirm |
| Expedited/compliance delete (GDPR right-to-erasure, B14) | **DESIRABLE** | Under GC lock, confirm no live ref, then delete bypassing the two-phase ack-floor graduation delay (the old grace_sec is gone); no layout change |
| Encryption-at-rest × content-addressing (B17) | **DESIRABLE** | Dedup scope per-encryption-key; local to key/hash derivation |
| Local / NFS / shared-fs as a first-class backend (B26, + B135 multi-mount safety) | **DESIRABLE** | Unit-tested over `LocalObjectStorage`; needs server-level doc + the put-if-absent atomicity caveat (racy multi-writer on local/NFS) + multi-mount safety notes |
| Namespace registry unbounded growth (B129) | **PARTIAL** (D1 fixes the create/drop fanout) | D1 shard-incarnation removes monotone fanout; registry itself removed by D1 |
| Shutdown hang in `clickhouse local` + CA disk (B48, + B167a/f graceful server shutdown wiring) | **TODO (release gate)** | GC thread / `BackgroundSchedulePool` not reaped on `LocalServer` exit; server-side graceful-shutdown ordering (stop scheduler -> release lease -> farewell beat) needs an explicit pass |
| Forbidden-term event names in `content_addressed_log` (B192) | **TODO** | Event type names review |

---

## Release readiness — first production release {#release-gates-2026-07-03}

Groomed 2026-07-03 (supersedes the 2026-06-24 grooming from the pre-consolidation backlog; the fold
had dropped several live items — B94/B98/B121/B202/B206/B207/B135/B169 — now restored above).

### REQUIRED before the first production release {#release-required}

Validation campaign (one coherent block):
1. **Real-S3 GC validation** — reclaim actually reclaims on AWS/GCS/Azure; LIST consistency of the
   token-diff discovery; rustfs is NOT a release-quality store (leak #3231 + false 404s).
   **AWS part DONE 2026-07-03** (bucket `test-altinity-support-team`, prefixes `ca_live_20260703_r1/r3`):
   probe passes (honest 412s), replication + `dedup_ratio=2` across two roots, two-phase reclaim
   verified against the bucket (73.8 MB -> 37.3 MB after merge-churn, -> 13.9 KB after `DROP TABLE`),
   fsck clean at every step, 0 anomalies/clamps/false-404s in 28+ rounds. Round duration on live S3
   is 30-40 s (vs ~5-10 s rustfs) — the default `gc_interval_sec=60` is sane. Fixes shipped from the
   run: probe empty-`If-Match` cleanup 400s; foreign-owner message; `blob_storage_log` conditional
   deletes; factory `root_shards` default 8->32; `server_root_id` macro expansion.
   **GCS part DONE 2026-07-03** (bucket `content-adressable-test-mfilimonov`, prefix `ca_live_20260703_g2`,
   `http_client = gcs_hmac` Generation binding): probe passes end-to-end (first CAS mount on GCS),
   replication + checksums, two-phase reclaim verified against the bucket (11 condemned -> graduated ->
   deleted; DROP-to-zero: 51 objects / 20.8 KB pool metadata), fsck all-zero, `dedup_ratio=2`,
   `blob_storage_log` records deletes. Three live-found fixes landed: numeric probe wrong-tokens
   (`1f58e7f2fef`), no LIST-derived tokens on generation stores — XML LIST bodies carry MD5 ETags
   (`86f44c8061c`), `Expect: 100-continue` sees `x-goog-if-generation-match` (`01b4b92a945`).
   **Remaining: Azure** (not started).
2. **Long chaos soak (4h+) on a compacting store** — confirms B165 (OOM at hour 4) resolved and the
   whole night-fix stack under sustained chaos; gated by B207 (below) for honest verdicts.
3. **B207 fsck phantom-dangling race fix** — release validation is only as честный as its oracle.
4. **ci/full-scale scenario sweep** — the dev-scale inconclusives (RSS attribution, manifest caps)
   must run at their designed scale at least once.
5. **Test debt that hides real bugs**: D3 (`gc_shards > 1` full-round tests), B5 (per-server-tree
   integration reconcile), the SIGSTOP-floor + request-budget scenario cards.
6. ~~TLA+ clamps + suppression extension~~ — **DONE 2026-07-03** (see the row in §GC protocol).

Feature/safety gates:
7. ~~**B1 `manifest_hash` on the Keeper `/parts` znode**~~ — **REJECTED 2026-07-14** (replication stays disk-agnostic; see `BACKLOG.md §obsolete`).
8. **B31 capability gate** — honest advertisement; reject unsupported ops at `CREATE`/`ATTACH`.
9. **B197 `SYSTEM` control surface** — START/STOP GC, POOL READONLY, CHECK.
10. **B13 migration path + mixed-version rollout rule** (read-new-before-write-new; the format
    self-check already fails closed) — users need a way in.
11. **Format freeze + B180 pool-format version breadcrumb** — first persisted-data release freezes
    the format (the schema-evolution framework is in place); stamp the pool self-describingly.
12. **B192 event/log naming review** — names freeze with the logs.

Operability/hygiene gates:
13. **B198 backup/restore runbook.**
14. **B48 (+B167a/f) clean shutdown** — `clickhouse local` hang + server graceful-shutdown ordering.
15. **B196 `s3_max_connections` cap to backend permits** (cheap; kills 503/retry storms).
16. **GC discovery O(N²) LIST fix** — ✅ DONE (2026-06-29..07-01): discovery LISTs flat `cas/refs/` not `roots/` (`f5f96dce01a`+`644eb7c6ade`) and backend `list` is real streamed pagination (`b15f1ef9d28`). Remaining O(universe)-per-round cost is the separate Phase-4 fold/discover skip-unchanged item.
17. **B3/B186 red `FreezeViaHardLinksIntoShadow` gtest** — RESOLVED 2026-07-11 (`ecb6e1a5e58`): tombstone-aware intermediate-dir `existsDirectory`. CA gtest battery fully green.
    via B31.
18. **B131 repo hygiene + the M-W comment sweep** — a clean upstream PR.

### DESIRABLE before release (not gating) {#release-desirable}

- T1 delta-runs + compaction (GC byte volume on hot pools); GC round progress watchdog.
- Per-namespace `root_shards`; adaptive shard splits (see their rows).
- GCS follow-ups (validation-grade -> production-grade): compose-based conditional finalize for
  blobs above `gcs_max_conditional_put_bytes`; `gcp_oauth` dialect probe validation (ADC creds);
  generation-aware LIST discovery (GC re-reads every shard on GCS since list tokens are disabled —
  cost only); signed `x-goog-*` extra_headers on `gcs_hmac` (currently unsigned, CAS configures none).
- B202 inline-by-size (+ threshold setting); one-GET part open; B121 per-blob-GET read cost.
- Streaming `putOverwrite` (B98 huge-blob displacement); promote-time recreate for SOURCED deps.
- B66b relink-into-detached; B66a local-storage concurrent-fetch atomicity.
- B15/B99/B169/B159 completion: per-part/ref `system.*` views + disks decode/introspect.
- 2026-07-06: introspection package landed — `system.content_addressed_mounts`, `mount_claim`/
  `mount_release`/`mount_conflict` audit events, first gauges (`CasGcIsLeader`,
  `CasGcPendingReclaimEntries`), and scoped/partial `fsck` (`--namespace`, `--timeout`/`--partial`);
  live-validated on the RustFS stand. See `docs/superpowers/plans/2026-07-06-cas-introspection-package.md`.
- B14 expedited/GDPR delete; B17 encryption-at-rest interaction; B26+B135 local/NFS first-class.
- Dedicated gc-round-log row for `rebuildBaseline`; `process_epoch` → `writer_epoch` unification;
  common-shard-prefix single-LIST discovery; fsck Orphan-class test gap.
- S12/S22/S27 scenario infra (multi-node Cluster abstraction; fault-injecting / LIST-instrumented
  S3 proxy); B206 soak settle-gate tuning.
- RustFS upstream reports: #3231 follow-through + the false-404-under-load repro.

---

## Testing {#area-testing}

See [`08-testing-and-soak.md`](08-testing-and-soak.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| Adversarial scenario suite S01–S35 | **PARTIAL** | 2026-07-03 night sweep (dev scale): 8 PASS, ZERO real fails (the one S13 'fail' was a card bug — oracle before sync; fixed, re-run PASS 11/11); all seven previously-FAILing scenarios clean; remaining inconclusives are honest scale gates ('rerun at ci/full') and infra gates (S12/S22/S27). NEXT: one ci/full-scale sweep |
| Pool-hash vs `checksums.txt` consistency test (2026-07-03) | **TODO** | A test proving the CAS blob hash convention (streaming chunked `CityHash128`, 2048-byte blocks) yields the same value as the part's `checksums.txt` entry for each file it covers — the dedup/adopt paths silently rely on this equivalence; a drift (e.g. a future hash change on either side) must fail a test, not corrupt dedup |
| S01 memory bloat fix (streaming `putBlob`) | **DONE** | Confirmed < 2x peak vs ~6.5x before |
| S33 concurrent-leader reclaim-leak guard | **DONE** (now a real regression guard) | Attempt-scoped generations fix means S33 PASS = no leak |
| S30/S34/S35 D1 regression guards | **DONE** | All PASS in D2 |
| Soak harness (green-path: workload + oracle + fsck checkpoints) | **DONE** | Runs clean |
| Soak harness (chaos phase) | **PARTIAL** | Basic chaos works; TTL-band oracle + large-pool fsck timeout limit long runs |
| 4h continuous chaos soak | **TODO** | Blocked by: compacting object store, streaming fsck, TTL-robust oracle |
| S12 (10-replica shared pool) | **TODO** | Requires docker-compose with 10 ClickHouse services |
| S22 (throttling/retry) | **TODO** | Requires fault-injecting S3 proxy |
| S24 (small dedup-cache) | **DONE** | smalldedupcache variant wired; night sweep PASS 9/9 |
| S27 (list pagination ambiguity) | **TODO** | Requires instrumented object-store proxy |
| S31 (`ca-gc-dryrun` under `gc_shards > 1`) | **DONE** | `previewDeletes` iterates every target shard (fixed during ack-floor); gc_shards2 variant in the night sweep 8/9 (scale-gated remainder) |
| D3: full GC round test under `gc_shards > 1` (edge-set fold) | **TODO** | Cover fold → retire → reclaim over the source-edge-set (D1) with multiple GC shards; `gc_shards=1` tests hide sharded fold bugs (see [[feedback_review_blindspots_shards_chassert]]) |
| B5: reconcile shared-pool integration tests to per-server-tree | **TODO** (separate/larger) | Integration tests still assume the old shared-pool layout; reconcile to the per-`server_root_id` tree (`cas/refs/<srid>`, `cas/manifests/<srid>`, `roots/<srid>`) after Phase 1 relocation |
| Stateless test suite gated CA un-tagging | **PARTIAL** | Most `no-content-addressed-storage` tags removed; remaining are feature gaps (B31 capability gate, B66a concurrent-fetch, freeze/WORM edge cases) |
| CA-s3 stateless lane drive-to-green (2026-07-10) | **DONE (point-fixes); full-lane run pending** | Fixed: `04286` (directory-safe HEAD), `05008` (ack-floor `entries_redeleted>=objects_deleted` invariant), `05009` (default-ON log + disk_name filter), `01271` (privilege ref), `03829` (150M→170M for the CA write buffer). Removed strays (untracked / wrong-branch): `03649/03650_alias_marker_distributed`, `test_optimize_using_constraints`. B86 removed (system logs on CA). Remaining reds are conclusively NOT CAS and to be IGNORED: arch x86-local-vs-arm-CI FP (`01854_s2`, `02224_s2`, `03233_dynamic`), local infra no-MySQL/no-IPv6 (`02479`, `01880`, `02784`), web-disk-not-CA (`04033_tpc_ds_q14/q24/q31`). Genuine-but-inherent: `00146` one-shot cacheless read. See [[reference_ca_s3_lane_ignore_tests]] |

---

## Deferred backlog summary {#deferred-backlog-summary}

The former standalone backlog (`deferred_backlog/cas-mergetree-integration.md`) is now folded into
this roadmap. The items below are the still-actionable highlights not already covered above:

- **B10 minor code-review findings** (low severity): `inDegreeInGeneration` O(candidates × runsize);
  unguarded `int64_t` in-degree accumulation before the `merged < 0` guard; `~Build` destructor
  calling `retireBuildSeq` without confirmed `noexcept`; `CasStore Resolved.manifest_size` always
  zero; redundant 2nd watermark GET in `sweepNamespace`. None are safety blockers.
- **B66b relink-into-detached** (desirable): same-pool `to_detached` FETCH currently byte-streams
  even though a zero-cost relink is possible; extend `Fetcher::relinkPartToDisk` to honor
  `to_detached`.
- **GC run-file streaming** (scalability): **DONE** (T2, 2026-07-02). `RunFileReader` now has a
  streaming mode over true ranged reads in `CasObjectStorageBackend::get` + the `getStream` seam,
  shared by all run consumers; the whole-run `full` member is gone. The follow-on byte-volume work
  (delta-runs + compaction, T1) stays DESIRABLE — see [`BACKLOG.md`](BACKLOG.md) §2 and
  `specs/2026-07-02-cas-gc-snapshot-streaming-design.md`. (The superseded scoping doc
  `deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md` was removed in the 2026-07-13 grooming.)
- **B1 manifest_hash on Keeper `/parts` znode**: **REJECTED (2026-07-14)** — no longer a gate.
  Replication code stays disk-agnostic (no CA-specific field in the Keeper part header); the
  manifest id travels in-band on fetch, and manifest divergence is benign
  (`01-architecture.md §benign-cross-replica-divergence`). See `BACKLOG.md §obsolete`.
- **S23 idle RSS +82 MiB over budget**: confirm not unbounded in a long soak run.
