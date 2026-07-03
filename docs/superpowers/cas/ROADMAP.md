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
section documents for technical detail. This document **is** the living backlog: the former
`deferred_backlog/cas-mergetree-integration.md` has been folded in here, organized by area.

Status stamps: **DONE** = implemented and validated; **PARTIAL** = core implemented, gaps remain;
**TODO** = agreed as necessary, not yet implemented; **DESIRABLE** = identified as valuable but
not committed; **REJECTED** = investigated and deliberately not pursued (reason recorded).

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
| File-cache disk over a CA disk (2026-07-03) | **TODO (release-relevant for prod read perf)** | `<type>cache</type>` over a CA disk throws `NOT_IMPLEMENTED` at startup: the cache disk's WRITE path uses the generic transaction (`generateObjectKeyForPath`, impossible for content-addressed keys) because `MetadataStorageFromCacheObjectStorage` forwards neither `isContentAddressed` nor the CA transaction surface (and `writeFileImpl`'s CA branch `dynamic_cast`s the transaction). Fix = teach the cache wrapper to expose/delegate the CA write path; read side needs no work (immutable content-hash keys cache perfectly; control-plane refs bypass the file cache by construction). Hit live on the operator test stand |
| Excise the retired `Tree` object kind from the code (2026-07-03) | **TODO (pre-freeze cleanup)** | The standalone `Tree` layer is retired (rev. 15 `PartManifest` redesign; docs already say not-implemented) but the code still carries it: `ObjectKind::Tree`, the `CATR` magic + envelope/codec remnants, `FormatId::Tree`, the `trees/` branch in `classifyCasNs`, `CasTree*` ProfileEvents (part manifests should count as `CasManifest*`, not Tree), `TreePut/TreeExpand/TreeRetire/TreeDelete/TreeStrip` event types, the `objectKey(..., ObjectKind::Tree, ...)` LOGICAL_ERROR thrower, and Tree-exercising unit tests. Remove before the B180 format freeze so the frozen surface has no dead kind. Coordinate with the `tree_delete`/`tree_retire` rows already emitted into `system.content_addressed_log` (rename or keep as historical enum values — decide during the pass) |
| Staging area inside the bucket (2026-07-03) | **TODO (needs brainstorm/spec)** | Support a dedicated staging prefix in the pool bucket: objects uploaded out-of-band (bulk load, backup restore, external tooling) land under staging and are ADOPTED into the pool via the verified copy-forward/adopt path (hash-verify, then publish) instead of being trusted in place; scope and exact semantics to be specced |
| Durable-monotone `writer_epoch` / `build_sequence` identity | **DONE** | Prevents stale-epoch cross-publish |
| Mutable vs immutable file classification | **DONE** | `.bin`, mark files, `primary.idx` → blobs; others → verbatim or inline |
| Streaming `Build::putBlob` (no whole-blob memory materialization) | **DONE** | Was materializing `BlobSource` into a `String`; now streams from staged temp file; peak memory ~2x instead of ~6.5x |
| Manifest soft/hard limits with backpressure (B164b) | **DONE** | `manifest_soft_limit` paces writes; `manifest_hard_limit` throws `LIMIT_EXCEEDED` before any ref is published |
| Precommit-first publish (durable precommit before blob upload) | **DONE** | Ensures abandoned precommits are visible to GC |
| Dedup cache (known-present blob cache) | **DONE** | Configurable `dedup_cache_bytes`; a hint only, never affects correctness |
| Adaptive HEAD-before-PUT (skip body upload for known-present blobs) | **DONE** | `dedup_head_first_min_bytes` threshold |
| Replication fetch-by-relink (same-pool part relink, zero byte cost) | **DONE** | `test_cas_replicated_relink` integration test passes |
| `manifest_hash` field on Keeper `/parts` znode (cross-replica header divergence detection) | **TODO** | `commitPart` / `getCommitPartOps` in `ReplicatedMergeTreeSink.cpp` have no CA-specific code yet |
| Streaming `putOverwrite` path (condemned-displacement case) | **DESIRABLE** | The rare INV-1 revival/displacement path still materializes whole body; not a blocker |
| `clickhouse local` shutdown hang (GC thread not reaped on local exit) | **TODO** | B48: background `BackgroundSchedulePool` / GC thread not joined on `LocalServer` teardown |
| Relink-into-detached fetch (zero-byte `to_detached` fetch for same-pool parts) | **TODO** | B66b: currently byte-streams; extend `Fetcher::relinkPartToDisk` to honor `to_detached` |
| Concurrent-fetch torn read of shared `detached` ref on local storage | **TODO** | B66a: `LocalObjectStorage` write is not atomic; safe on S3 (atomic PUT) |

---

## GC protocol {#area-gc}

See [`04-gc-protocol.md`](04-gc-protocol.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
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
| Delta-runs + compaction for the snapshot (bytes O(edges)/pass) | **DESIRABLE** (T1) | The HOT-pool full snapshot rewrite per pass is the next dominant byte cost; builds on the T2/T0 primitives (streaming reader with `seek`, `getStream`, ranged `get`, seal-ref resolution) unchanged. NEXT spec; source `specs/2026-07-02-cas-gc-snapshot-streaming-design.md §what-deliberately-does-not-change` |
| GC round progress observability (round-duration watchdog, LIST/window progress events, alert on `gc_fold_begin` without `gc_fold_end`) | **TODO** | Motivated by the 2026-07-02 soak forensics: a long/wedged round is currently only visible after the fact; emit a round-duration watchdog + LIST/fold-window progress events + an alert on an unbalanced `gc_fold_begin`/`gc_fold_end` pair |
| `process_epoch` → `writer_epoch` stamp unification | **DESIRABLE** | The writable path already sets `process_epoch = writer_epoch`; unify the manifest `writer_instance_id` stamps |
| Clamp-suppressed GC passes (no graduation / no pending deletes while any shard is clamped) | **DONE** | 2026-07-03 night SAFETY fix (`c47d10d01ec`): clamps break the ack-floor lemma 'landed before the cut => folded before graduation' (the model's SabotageSkipChangedShard, realized — 31 dangling in the night soak, caused by RustFS false 404s under the #3231 storm); a clamped pass carries everything, deletes resume on the first clamp-free pass; `gc_fold_clamp` event per clamp. See `04-gc-protocol.md §absent-at-head` |
| TLA+ model extension: clamps + destruction suppression | **DONE** | 2026-07-03: honest clamp in `GFold` (fold may hold back one landed ref but DECLARES it via `clampedL` — vs the still-lethal undeclared `SabotageSkipChangedShard`), suppression guard in `GComplete` grads, `SabotageClampNoSuppress` reproduces the night's INV_NO_DANGLE counterexample, `W_ClampHappens` witness fires; honest stage-1 CLEAN (83.9M distinct states); all 10 prior sabotages + 6 witnesses re-verified |
| B207 fsck consistency race (phantom dangling under concurrent GC) | **TODO (release gate)** | Restored from the pre-consolidation backlog (lost in the fold): `runFsck`'s ref-walk and HEAD-confirm are minutes apart with no snapshot — a re-published ref + a legitimate GC delete manufactures a false `dangling`. FIX: at the HEAD-absent branch, RE-RESOLVE the referencing ref(s) (labels already collected); only a CURRENT ref over an absent object is dangling. Gates honest release-validation soaks (B185/B206/B144 were all this race) |
| Verified copy-forward for condemned tokenless-evidence deps at the promote gate | **DONE** | 2026-07-02, `specs/2026-07-02-cas-copy-forward-condemned-evidence.md`: fixes the S13 soak-run-3 attach brick (`republishRef` -> promote `ABORTED` -> table readonly forever). Narrow INV-1 exception (committed-source evidence only; full content verification; token-conditional `putOverwrite`); TLA+ `WCopyForward` gate; `blob_copy_forward` event + `CasBlobCopyForward` counter. See [`03-writer-protocol.md`](03-writer-protocol.md) |
| Promote-time in-place recreate of a condemned SOURCED blob | **DESIRABLE** | For tokened (sourced) deps the promote gate stays fail-closed `ABORTED` (build-local sources not retained at promote); recreate happens on the retried build via `putBlob` cold-reuse. The tokenless-evidence case is DONE (copy-forward above) |
| fsck pipeline classification (`pending-gc` / `awaiting-gc` / `unaccounted` replace the suspicious `unreachable` lump for blobs; de-alarm notes) | **DONE** | 2026-07-03, from the raw-audit RFC triage: deletion lag of the two-phase pipeline is now labeled as the expected state it is; `unaccounted` (outside the whole GC view) is the anomaly signal (INV-2). See `08-testing-and-soak.md §fsck` |
| Raw GC rebuild (`gc/state` disaster recovery) | **DONE** | 2026-07-03, `specs/2026-07-03-cas-gc-rebuild-design.md`: the "план Б" survivor of the 2026-06-30 raw-audit RFC. A fail-closed baseline guard (`CORRUPTED_DATA` when a shard journal proves trimmed history with no healthy adopted seal) ships ahead of `Gc::rebuildBaseline(force)` — derived-bookkeeping only, over-protect only (synthetic baseline from owner replay + EMPTY retired lists + round minted above every surviving mount ack/fence-round/generation), single `gc/state` CAS. Surfaced as `SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] [<disk>]` and `clickhouse-disks ca-gc-rebuild [--force]` (read-only-open required). Registry-repair/orphan-sweep/debris-prune parts of that RFC are obsolete (registry removed by D1; sweeps live in regular rounds; no structural orphan-blob class per INV-2). See `04-gc-protocol.md §gc-rebuild`, `08-testing-and-soak.md §gc-rebuild-runbook` |
| B94 full-GC/check backstop for physical debris/drift | **DONE** (by composition) | `clickhouse-disks fsck` (pipeline-classified) + `ca-gc-dryrun` + the raw GC rebuild cover the audit/backstop surface; regular GC stays incremental by design (raw-audit RFC non-goal) |
| RustFS false-404-under-load upstream report (HEAD returns 404 for live objects while the metacache is degraded by rustfs#3231 dir bloat; caused the 2026-07-03 clamp era) | **TODO** | Build a repro on top of the rustfs#3231 repro (bloated dirs + concurrent stat); our side is already safe (clamp + destruction suppression, `04-gc-protocol.md §absent-at-head`) |
| Per-namespace `root_shards` (chosen at table creation) | **DESIRABLE (next)** | 2026-07-03 weighing: GC needs no N (`discoverUniverse` LISTs, the fold digests what exists) — only the owning writer's/readers' `shardOf` does, so a per-namespace meta object (putIfAbsent at first publish, immutable) + a DDL hint (`SETTINGS cas_root_shards=N`) suffices. Payoff is at the 100k-table scale: discovery keys ∝ Σ N over all tables — cold tables at N=1-4 collapse it while hot tables keep 32-64. No resharding: a wrong guess lives like today's pool constant, per-table |
| Adaptive shard SPLITS (hash-prefix radix, writer-local) | **DESIRABLE** | Avoids the killer (mass cross-shard renames = two-owner/zero-owner windows): N is a power of two, shard = top hash bits; splitting hot shard k into k0/k1 partitions refs deterministically by the next bit — a LOCAL single-writer op. Load-bearing precondition: split only when the shard is FULLY FOLDED AND TRIMMED (cursor == shard_version, no clamps, no live precommits) so children start with empty journals; incarnation stamps already handle new-object-at-path (ABA); GC edges unaffected (source_id is shard-independent). Reader routing = radix over shard object names ("0","10","11") with a self-healing re-LIST cache. The writer feels its own heat (flush latency = body size) and splits itself; merge-back for cold shards by the same shape. Real cost: spec + TLA+ for split x fold/trim/precommit interleavings + fsck awareness. Main prize: the cold tail of large installations + insurance for wrong per-table hints |
| Sweep dangling `M-W`/plan references from code comments | **TODO** | ~15 comments reference the deleted `plans/2026-06-12-ca-core-m-w.md` ("M-W T3", "D-W1", "M-W design section 4") — replace with self-contained wording or `docs/superpowers/cas/` pointers |
| Shard-mutation flat-combining queue (group commit per ref shard) | **DONE** | 2026-07-03, `specs/2026-07-03-cas-shard-mutation-queue.md`: soak measured 637k casPut attempts for 380k landed (40% conflicts, 92% under storms, each conflict re-reading ~280 KB) from up to 156 concurrent mutating threads over 64 shard keys with NO intra-server serialization; the queue makes intra-server conflicts structurally impossible and compresses bursts into single casPuts (fewer rustfs#3231 leaks until upstream fixes). See `03-writer-protocol.md §shard-mutation-queue` |
| Writer/mount introspection insights | **DONE** | 2026-07-02: `mount_beat` event on every view ADVANCE (installed round, prior round, retired-entry count loaded), `mount_remount` event (ok/failed) from the self-remount loop, and the GC round log gained the ack-floor pipeline columns (`entries_condemned/graduated/redeleted`, `fence_outs`, `min_ack`, `anomalies` — replacing the dead always-0 cascade/forget columns) |
| GC discovery O(N²) LIST quadratic over `roots/` | **TODO** | `listRootShardTokens` re-enumerates the whole prefix per page; fix: real paginated list at backend |
| Common-shard-prefix for GC discovery (IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST) | **DESIRABLE** | Relocate shard objects to one flat prefix → GC discovery = a single LIST; pre-release layout change is free |
| Run-file O(buffer) streaming | **DONE** (T2, 2026-07-02) | `RunFileReader` streaming mode (borrowed-memory + streaming); true ranged reads in `CasObjectStorageBackend` + `getStream` seam; the whole-run `full` member is gone. See the T2 row under [GC protocol](#area-gc) |
| `inDegreeInGeneration` O(candidates × runsize) | **RESOLVED** by the ack-floor round | The per-candidate recheck whole-run re-read is gone — the retired cursor rides the single three-cursor merge; the function remains only for preview/tests |
| `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION [<disk>]` command | **DONE** | Synchronous explicit GC trigger; logs to `system.content_addressed_garbage_collection_log` |
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
| Real-S3 GC validation (confirm GC deletes actually reclaim on production AWS/GCS/Azure) | **AWS DONE 2026-07-03**; GCS/Azure TODO (HARD release gate) | AWS: probe/replication/dedup/two-phase reclaim/DROP-to-zero all verified live (see §release-required #1); rustfs beta.8 does not compact tombstones |
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
| CREATE/load empty-namespace HEAD storm (2026-07-03, operator stand) | **TODO (prod gate)** | `Store::listRefs` HEADs ALL `root_shards` (32) per `existsDirectory`/`listDirectory` on an empty namespace, absence is never cached (`readShardDecoded` erases on 404) — a fresh `CREATE TABLE` costs 3 sweeps = ~102 HEAD-404s (20 s at the stand's 175 ms/op); same cost per table on EVERY server restart. Fix package: (c) answer namespace scans from ONE prefix LIST of `cas/refs/<ns>/` instead of N HEADs + memoize namespace-empty; (b) short-TTL negative shard cache invalidated by namespace writes (exists is non-destructive — the 404-invariant applies to destructive paths only). Classifier half fixed same day (`classifyCasNs` knew only pre-relocation paths) |
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
| `manifest_size` field in `Resolved` always 0 | **TODO** (minor, B10) | `resolveRef` never sets it; harmless but imprecise |
| Replication fetch-by-relink (zero byte cost for same-pool parts) | **DONE (base)** | `manifest_hash` on Keeper `/parts` znode still TODO |

---

## Operability and release readiness {#area-operability}

| Item | Status | Notes |
|------|--------|-------|
| `system.content_addressed_garbage_collection_log` | **DONE** | Per-round GC audit log |
| `system.content_addressed_log` | **DONE** | Per-event CA audit log (off by default) |
| `clickhouse-disks fsck` | **DONE** | Independent pool reachability verification |
| `clickhouse-disks ca-gc-dryrun` | **DONE** | GC delete preview (zero writes) |
| Read-only disk mode (WORM deployment) | **DONE** | |
| `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION` command | **DONE** | |
| Capability gate: reject unsupported ops at `CREATE`/`ATTACH` with clear error (B31) | **TODO** (HARD) | Currently `supportsHardLinks` / `supportZeroCopyReplication` advertise wrong capabilities |
| `SYSTEM` control commands: START/STOP GC, POOL READONLY, CHECK (B197) | **TODO** (HARD) | |
| `system.*` views for pool/blob/part refcounts + GC status + frozen snapshots (B15/B99/B169/B159) | **PARTIAL** | GC log + event log + fsck/dryrun/rebuild CLI done; per-part/ref views and a `clickhouse-disks` decode/introspect (top-down traversal) surface not yet |
| Backup/restore runbook (B198) | **TODO** (HARD) | |
| Pool-format version breadcrumb (B180) | **TODO** | Self-describing pool meta for version identification |
| Integration tests on RustFS (not MinIO) (B125) | **TODO** (HARD) | Current integration tests use MinIO; production uses S3-compatible backends |
| Repo hygiene — non-shippable files removed from the diff (B131) | **TODO** (HARD) | Blocks a clean upstream PR |
| `CaWiringOps.FreezeViaHardLinksIntoShadow` gtest failure (B3 / B186) | **TODO** (HARD) | `removeRecursive("shadow/bk1")` + commit leaves `existsDirectory("shadow/bk1")` true; one red gtest in the CA battery |
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
   **Remaining: GCS + Azure** (the GCS `Generation` token binding is still fail-closed-unprobed).
2. **Long chaos soak (4h+) on a compacting store** — confirms B165 (OOM at hour 4) resolved and the
   whole night-fix stack under sustained chaos; gated by B207 (below) for honest verdicts.
3. **B207 fsck phantom-dangling race fix** — release validation is only as честный as its oracle.
4. **ci/full-scale scenario sweep** — the dev-scale inconclusives (RSS attribution, manifest caps)
   must run at their designed scale at least once.
5. **Test debt that hides real bugs**: D3 (`gc_shards > 1` full-round tests), B5 (per-server-tree
   integration reconcile), the SIGSTOP-floor + request-budget scenario cards.
6. ~~TLA+ clamps + suppression extension~~ — **DONE 2026-07-03** (see the row in §GC protocol).

Feature/safety gates:
7. **B1 `manifest_hash` on the Keeper `/parts` znode** — cross-replica header-divergence detection.
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
16. **GC discovery O(N²) LIST fix** — self-inflicted scale DoS; cheap (real pagination).
17. **B3/B186 red `FreezeViaHardLinksIntoShadow` gtest** — fix or explicitly waive FREEZE support
    via B31.
18. **B131 repo hygiene + the M-W comment sweep** — a clean upstream PR.

### DESIRABLE before release (not gating) {#release-desirable}

- T1 delta-runs + compaction (GC byte volume on hot pools); GC round progress watchdog.
- Per-namespace `root_shards`; adaptive shard splits (see their rows).
- B202 inline-by-size (+ threshold setting); one-GET part open; B121 per-blob-GET read cost.
- Streaming `putOverwrite` (B98 huge-blob displacement); promote-time recreate for SOURCED deps.
- B66b relink-into-detached; B66a local-storage concurrent-fetch atomicity.
- B15/B99/B169/B159 completion: per-part/ref `system.*` views + disks decode/introspect.
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
  (delta-runs + compaction, T1) stays DESIRABLE. See
  `specs/2026-07-02-cas-gc-snapshot-streaming-design.md`; the superseded plan is
  `deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`.
- **B1 manifest_hash on Keeper `/parts` znode** (HARD release gate): cross-replica header-divergence
  detection requires a CA-specific field in `commitPart` / `getCommitPartOps`.
- **S23 idle RSS +82 MiB over budget**: confirm not unbounded in a long soak run.
