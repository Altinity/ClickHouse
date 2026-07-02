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
| Ack-floor round soak validation | **TODO** | Kill-mid-burst spare-then-recondemn (no dangle); SIGSTOP writer holds then releases the floor; O(delta)+O(servers) request-count regression guard. See [`08-testing-and-soak.md §backlog`](08-testing-and-soak.md) |
| Self-remount on GC fence-out (a fenced live server re-opens a FRESH incarnation via the S13 mount machinery: epoch bump + reclaim + fresh view; today it is write-fenced until a server restart) | **TODO** | Found by the 2026-07-02 ack-floor soak: a 51s CH pause + concurrent GC round fenced the mount; the keeper fails closed by design (sleeper re-arm is the TLA+ sabotage), liveness needs the fresh-incarnation path. Interim: ca-soak chaos caps CH pauses at 20s. Also soften the `Store` teardown release message for the `gc_fenced` case (a foreign incarnation there is the EXPECTED fence-out outcome, not corruption) |
| Ack-floor observability (Task 11) | **DONE** | `CasGcRetired{Condemned,Spared,Graduated,Redeleted}` + `CasGcHeartbeatFenceOuts` + `CasGcFloorHeldByStaleAck` ProfileEvents; `gc_fence_out` audit event; WARNING when a live heartbeat's ack lags the round by > 2; `RoundReport` carries condemned/graduated/redeleted/fence_outs/min_ack |
| `mayMutate` fence deadline on `CLOCK_BOOTTIME` (Task 12) | **DONE** | `steady_clock` does not advance across a VM suspend; the write-fence deadline is now a `CLOCK_BOOTTIME`-ms instant (`Store::bootMs`, injectable via `PoolConfig::boot_ms_fn`) so a resumed VM sees its fence expired (container pause already safe) |
| Delta-runs + compaction for the snapshot (bytes O(edges)/pass) | **DESIRABLE** | With cheap frequent rounds, the full snapshot rewrite per pass is the next dominant cost; the deferred O(buffer) streaming-merge work (`deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`) |
| `process_epoch` → `writer_epoch` stamp unification | **DESIRABLE** | The writable path already sets `process_epoch = writer_epoch`; unify the manifest `writer_instance_id` stamps |
| Promote-time in-place recreate of a condemned blob | **DESIRABLE** | Today the promote gate stays fail-closed `ABORTED` (build-local sources not retained at promote); recreate happens on the retried build via `putBlob` cold-reuse |
| GC discovery O(N²) LIST quadratic over `roots/` | **TODO** | `listRootShardTokens` re-enumerates the whole prefix per page; fix: real paginated list at backend |
| Common-shard-prefix for GC discovery (IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST) | **DESIRABLE** | Relocate shard objects to one flat prefix → GC discovery = a single LIST; pre-release layout change is free |
| Run-file O(buffer) streaming | **DESIRABLE** (deferred) | `RunFileReader` materializes whole run in memory; two-cursor merge is streaming but inputs are not; fix requires ranged reads in `CasObjectStorageBackend` + streaming `RunFileReader` interface |
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
| Real-S3 GC validation (confirm GC deletes actually reclaim on production AWS/GCS/Azure) | **TODO** (HARD release gate) | rustfs beta.8 does not compact tombstones; real-S3 behavior must be validated before release |
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
| Root-shard fan-out vs per-object permit cap (B158: raise `root_shards`) | **TODO** | Reduces CAS contention at high insert rate |
| `RENAME` = one Build/part (B111) | **DESIRABLE** | Currently multiple root-shard updates per rename |
| Ack-floor round (removes the O(universe) fence + recheck phases) | **DONE** | The per-round all-shard fence is gone; dirty-only-fence (P6) is superseded. See `07-s3-budget.md §gc-budget` |

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
| `system.*` views for pool/blob/part refcounts + GC status + frozen snapshots (B15/B99) | **PARTIAL** | GC log and event log done; per-part/ref views not yet |
| Backup/restore runbook (B198) | **TODO** (HARD) | |
| Pool-format version breadcrumb (B180) | **TODO** | Self-describing pool meta for version identification |
| Integration tests on RustFS (not MinIO) (B125) | **TODO** (HARD) | Current integration tests use MinIO; production uses S3-compatible backends |
| Repo hygiene — non-shippable files removed from the diff (B131) | **TODO** (HARD) | Blocks a clean upstream PR |
| `CaWiringOps.FreezeViaHardLinksIntoShadow` gtest failure (B3 / B186) | **TODO** (HARD) | `removeRecursive("shadow/bk1")` + commit leaves `existsDirectory("shadow/bk1")` true; one red gtest in the CA battery |
| Migration path for existing tables (B13) | **TODO** | `ALTER TABLE … MOVE PARTITION` to a `content_addressed` disk re-packs; rollout safety spec needed |
| Server OOM at hour-4 soak (~49 GiB RSS, B165) | **TODO** (HARD) | Not reproduced since the `putBlob` streaming fix; re-run long soak to confirm |
| Expedited/compliance delete (GDPR right-to-erasure, B14) | **DESIRABLE** | Under GC lock, confirm no live ref, then delete bypassing grace; no layout change |
| Encryption-at-rest × content-addressing (B17) | **DESIRABLE** | Dedup scope per-encryption-key; local to key/hash derivation |
| Local / NFS / shared-fs as a first-class backend (B26) | **DESIRABLE** | Unit-tested over `LocalObjectStorage`; needs server-level doc and multi-writer atomicity note |
| Namespace registry unbounded growth (B129) | **PARTIAL** (D1 fixes the create/drop fanout) | D1 shard-incarnation removes monotone fanout; registry itself removed by D1 |
| Shutdown hang in `clickhouse local` + CA disk (B48) | **TODO** | GC thread / `BackgroundSchedulePool` not reaped on `LocalServer` exit |
| Forbidden-term event names in `content_addressed_log` (B192) | **TODO** | Event type names review |

---

## Testing {#area-testing}

See [`08-testing-and-soak.md`](08-testing-and-soak.md) for full detail.

| Item | Status | Notes |
|------|--------|-------|
| Adversarial scenario suite S01–S35 | **PARTIAL** | 14 PASS / 10 INCONCLUSIVE / 8 FAIL (D2); all FAILs are harness/infra/scale |
| S01 memory bloat fix (streaming `putBlob`) | **DONE** | Confirmed < 2x peak vs ~6.5x before |
| S33 concurrent-leader reclaim-leak guard | **DONE** (now a real regression guard) | Attempt-scoped generations fix means S33 PASS = no leak |
| S30/S34/S35 D1 regression guards | **DONE** | All PASS in D2 |
| Soak harness (green-path: workload + oracle + fsck checkpoints) | **DONE** | Runs clean |
| Soak harness (chaos phase) | **PARTIAL** | Basic chaos works; TTL-band oracle + large-pool fsck timeout limit long runs |
| 4h continuous chaos soak | **TODO** | Blocked by: compacting object store, streaming fsck, TTL-robust oracle |
| S12 (10-replica shared pool) | **TODO** | Requires docker-compose with 10 ClickHouse services |
| S22 (throttling/retry) | **TODO** | Requires fault-injecting S3 proxy |
| S24 (small dedup-cache) | **TODO** | Requires disk config variant |
| S27 (list pagination ambiguity) | **TODO** | Requires instrumented object-store proxy |
| S31 (`ca-gc-dryrun` under `gc_shards > 1`) | **TODO** | `previewDeletes` previews only shard 0; multi-shard coverage needed |
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
- **GC run-file streaming** (scalability, deferred): `RunFileReader` materializes the full run;
  fix requires real ranged reads in `CasObjectStorageBackend::get` + a streaming `RunFileReader`
  interface shared by all run consumers. Three-layer change; pick up with `superpowers:writing-plans`
  + TDD when scale demands it.
  See `deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`.
- **B1 manifest_hash on Keeper `/parts` znode** (HARD release gate): cross-replica header-divergence
  detection requires a CA-specific field in `commitPart` / `getCommitPartOps`.
- **S23 idle RSS +82 MiB over budget**: confirm not unbounded in a long soak run.
