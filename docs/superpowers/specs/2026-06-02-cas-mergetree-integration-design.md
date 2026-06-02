---
description: Design spec for integrating a content-addressed shared-storage layout for MergeTree into ClickHouse, milestone 1.
sidebar_label: 'CAS MergeTree integration'
sidebar_position: 1
slug: /superpowers/specs/cas-mergetree-integration
title: 'Content-Addressed MergeTree — Integration Design (Milestone 1)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Integration Design (Milestone 1) {#cas-mergetree-integration}

**Status:** design spec, awaiting review.
**Date:** 2026-06-02.
**Sources:** `content_addressed_shared_mergetree_design.md` (v3, the storage model + three adversarial review rounds), the standalone PoC `poc/cas_mergetree/` (the algorithmic oracle), and the deferred backlog `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (the running dead-end ledger, items B1–B12).

## 1. Goal and scope {#goal-and-scope}

Integrate a **content-addressed shared-storage layout** for MergeTree into ClickHouse as a clean replacement for zero-copy replication, without creating new architectural problems. Per the agreed approach we **design the overall integration just enough to prove no dead-ends**, then **fully spec milestone 1 (M1)** — a minimal but *truly content-addressed* per-disk layout for a non-replicated `MergeTree` on object storage.

**M1 delivers:** self-describing layout (no local-link indirection), content-addressed blobs with **intra-pool dedup**, **mutation carry-forward by reference**, a **global per-disk content pool**, and a **deferred reachability GC** — with zero changes to the part classes, `MergeTreeData`, or `StorageMergeTree` lifecycle.

**M1 explicitly defers** (all in the backlog, each with a plug-in point): replication / Keeper-backed refs (B1), distributed multi-writer GC coordination (B11), one-GET packing (B10), persisted/sharded refcount at extreme scale (B9), FREEZE DDL + materialized export (B4), projections / patch parts (B5), external-part ATTACH ingestion (B12).

## 2. Key decisions (from the design dialogue) {#key-decisions}

1. **Seam = a new `metadata_type`** (`IMetadataStorage` subclass), selected per-disk in the storage policy — the established `plain_rewritable` extension path. No new engine, no part-class changes. (Approach A1.)
2. **M1 is content-addressed *with* intra-pool dedup**, which makes removal reachability-governed — accepted.
3. **`remove` is a pure pointer-unlink and always safe**; blobs are reclaimed by a **deferred reachability GC**, never deleted inline. No `CanRemoveCallback` keep-subset computation.
4. **`parts/` + `blobs/` are a GLOBAL per-disk pool from day one** (retrofitting global onto per-table is architecturally expensive). GC reachability is therefore global.
5. **The part descriptor is addressed by the part checksum**, blobs by their file checksum — reuse existing `checksums.txt`, hash nothing new.
6. **Build local, then upload content-addressed** — S3 has no rename, so the checksum must be known before the only PUT; this also decouples GC `grace` from merge duration.
7. **Three interface seams** are reserved so deferred work needs no rework: `RefCatalog`, GC-coordination/lock, `BlobRefIndex`.

## 3. Architecture and components {#architecture}

New code (all behind one new `metadata_type = content_addressed`):

- **`ContentAddressedMetadataStorage`** (`IMetadataStorage`): path→object resolution (`getStorageObjects`), directory listing (drives part discovery), size/exists; **content-hash blob keys assigned on write** (build-local-then-upload); the per-part **footer**; owns the `RefCatalog` and `BlobRefIndex`. Lazy: LRU footer cache; refs listed once at load.
- **`Manifest`/footer** (de)serializer: the per-part descriptor — `sorted[logical_file → (blob_checksum, size, checksum)]` plus the deterministic small files; content-addressed by `part_id`.
- **Deferred GC sweeper**: background, per-disk, delta-driven mark-and-sweep with grace.
- **`RefCatalog`** (interface): list/put/drop refs. M1 impl = S3 objects under `store/<uuid>/{refs,detached,frozen}/`. Replicated impl (B1) = Keeper `/parts`.
- **`BlobRefIndex`** (interface): blob/part refcount. M1 impl = in-memory; scale impl (B9) = RocksDB + prefix-sharded reconciliation.
- **GC coordination/lock** (interface): M1 impl = in-process mutex (reuse `grab_old_parts_mutex`); multi-writer impl (B11) = Keeper leader-election + ephemeral lock + re-validate-at-delete.
- **Pool coordinator + self-check**: a `_pool_meta` record (pool format version, `coordination = none | keeper` + Keeper path, owner/leader lease); validates config + detects concurrent mounters at startup/periodically and **fails closed** on conflict. The `keeper` mode plugs the GC-coordination + `RefCatalog` Keeper impls in for any multi-mounted pool (replication being one consumer).

Reused unchanged: `MetadataStorageFactory` + `metadata_type=` config; part discovery (`loadDataParts` → `disk->iterateDirectory`); read (`prepareRead` → `getStorageObjects` → `FileCache`); commit (`renameTempPartAndReplace` → `IDataPartStorage::rename`); removal (`grabOldParts` → `IDataPartStorage::remove` → `removeSharedRecursive(keep_all_batch_data = true)`); in-process reader protection (`isSharedPtrUnique`); **all part classes (Wide/Compact), `MergedBlockOutputStream`, `MergeTreeData`, `StorageMergeTree`.**

## 4. Object layout {#object-layout}

```
<disk_root>/                               one disk = one pool = one GC coordinator
  blobs/<h0>/<h1>/<file_checksum>          GLOBAL, immutable; one part file's bytes (.bin/marks/columns.txt/primary.idx/…)
  parts/<h0>/<h1>/<part_id>                GLOBAL, immutable; the footer (file→blob checksum) + deterministic small files
  _pool_owner                              ownership lease (concurrency guard)
  store/<serverid>/                        per server/replica (distinct refs in a shared pool)
    <table_uuid>/
      refs/<part_name>          → { part_id, columns_hash(header), mutable fields }   active   (GC root)
      detached/<detached_name>  → part_id                                            detached (GC root, not active)
    frozen/<snapshot>/<table_uuid>/<part>  → part_id                                 frozen   (GC root, table-lifetime-independent)
```

- `<h0>/<h1>` = hash-prefix fan-out (S3 per-prefix rate limits; enables prefix-sharded reconciliation).
- **`part_id`** = `MergeTreeDataPartChecksums::getTotalChecksumUInt128` computed over the **deterministic file subset** — excluding `uuid.txt`, `txn_version.txt`, `metadata_version.txt` (random/mutable per replica). Those excluded/mutable fields live in the ref.
- **blob key** = the file's existing `checksums.txt` cityHash128 **+ size** (collision guard, verified on read).
- **refs** are the only mutable records and the listable directory entries, namespaced **per server/replica** (`store/<serverid>/<table_uuid>/`) so multiple writers' refs never collide and the GC can mark the **union across serverids** (this is the replicated per-replica `/parts`, B1). **`parts/`+`blobs/`** are content-addressed and **shared across the whole pool** (this is what makes replication no-copy).
- **frozen** lives at `store/<serverid>/frozen/<snapshot>/<table_uuid>/` — *outside* the table's data path — so a freeze has an **independent lifetime** (survives `DROP TABLE`, like local `shadow/`); it remains a GC root.
- **The footer and ref are versioned, extensible formats** (the core expandability rule). Because pool objects are immutable, deferred features extend the formats *additively* without breaking existing objects: the **footer** reserves a `projections` section (nested `part_id`s, B5) and patch-part references; the **ref** reserves the full `ReplicatedMergeTreePartHeader` (`columns_hash` + `MinimalisticDataPartChecksums`, required for B1 cross-replica divergence detection) plus the mutable `txn_version`/`metadata_version`. M1 writes the minimal version (`part_id` + columns_hash + size); later milestones bump the format version additively. A reader rejects a format version it does not understand (fail-closed).
- **Hierarchy** is Git-shaped: `ref (name→part_id) → part (part_id→file checksums) → blob (checksum→bytes)`.

## 5. Data flow {#data-flow}

M1 **reuses `StorageMergeTree`'s in-memory active-set / covering / DROP** unchanged — refs are just the on-store directory entries that `iterateDirectory` lists. (Deriving the active set from refs+covering and persisted tombstone-refs is the replicated story → B1.)

- **INSERT/MERGE/MUTATE write** — the part is **built in local scratch**, where `HashingWriteBuffer` already computes each file's checksum as it writes (the merge runs entirely locally; S3 sees nothing). On commit (the temp→active `rename`): **upload each file directly to `blobs/<checksum>`** via `putIfAbsent` (idempotent, dedups — no S3 rename/copy because the checksum is known first), write the `parts/<part_id>` footer, publish the ref. Local scratch ≤ part size during the build (served by the local/`FileCache` volume).
- **READ** — `iterateDirectory(refs)` → part names (existing discovery); file read → `getStorageObjects(part/file)` → ref → `part_id` → footer → `blobs/<csum>` (whole object) → existing read + `FileCache` stack. Footer cached. (One-GET open = B10.)
- **MUTATION carry-forward** — `MutateTask` is **unchanged**; its existing `createHardLinkFrom` for unchanged columns *means*, on this metadata type, "the new footer entry points at the same blob checksum" — reference, no re-upload, no new object. Changed columns are written fresh. New `part_id`, new footer mixing old+new checksums, new ref. The source stays live through the mutation, so its carried-forward blobs are continuously reachable.
- **REMOVAL** — `grabOldParts` → `IDataPartStorage::remove`. The content-addressed metadata storage's `remove` deletes **only the ref object** and emits the **deref-delta** (−1 each blob in the part's footer) to the `BlobRefIndex`; the global `parts/`+`blobs/` are kept (content-addressed, GC-managed). This is the *spirit* of `keep_all_batch_data = true`, but the metadata storage controls exactly what is removed (the ref) vs kept (the pool) — the generic flag keeps *all* remote, which would wrongly keep the ref too. Always safe; the part vanishes from `iterateDirectory`, never rediscovered.
- **DROP PARTITION/PART** — existing `StorageMergeTree` path removes covered parts → `remove` → drops refs. Blobs reclaimed by GC.
- **DETACH/ATTACH** — move the ref between `refs/` and `detached/` (metadata only; content untouched; detached stays a GC root). External-part ATTACH (ingest foreign bytes) → B12.

## 6. Garbage collection {#garbage-collection}

A background, per-disk, periodic sweeper over the global pool.

- **Mark roots** = `refs/ ∪ detached/ ∪ frozen/` across all tables (via `RefCatalog`). Reachability is hierarchical: a `part_id` is reachable iff a ref points at it; a blob iff some reachable footer lists it.
- **Steady state is delta-driven, never a full `LIST`.** The `BlobRefIndex` is maintained by part lifecycle events only: on commit, read the new footer and `+1` each blob/part; on ref-drop, `−1`. GC sweeps **refcount-0 objects past `grace`**. Cost = O(parts dropped since last pass × footer size) + the actual `DeleteObjects`.
- **Delta-driven requires a single coordinator with a *complete* view of the pool's ref changes** — this is the load-bearing assumption. A per-process refcount that sees only its own commits/drops is wrong as soon as a second writer exists. Three regimes: **(M1, single process)** the only writer maintains the `BlobRefIndex` from its own events — complete by construction. **(coordinated multi-writer, B11)** the global delta feed already exists as the Keeper replication **`/log`** + per-replica `/parts` watches (every add/remove across all replicas); the **single-leader GC** consumes it incrementally — still no full `LIST`; the refcount is the leader's rebuildable derived cache. **(uncoordinated multi-writer)** no global delta feed exists → delta-driven is impossible safely → **fail-closed via `_pool_owner`**. The full prefix-sharded reachability scan is the rare reconciliation/recovery path, never the steady state.
- **`grace` is measured from loss of reachability** (`first_unreachable[obj]` per pass; reachable ⇒ erased), and only needs to exceed the **longest part-upload window** (not merge/query duration — the merge is local). It is therefore small, which also keeps dropped-data retention short.
- **Reconciliation** = a rare, prefix-sharded full reachability scan that rebuilds the `BlobRefIndex` from S3 ground truth (after crash / on drift audit / DR). The refcount is a *derived cache*; S3 is truth.
- **Re-validate reachability at delete** (under the in-process lock for M1) so a just-committed ref is always seen.

**Safety (M1, single process), four cases:**
1. **In-process readers** — protected by the existing `isSharedPtrUnique` (a ref isn't dropped while a `DataPartPtr` is held) → reachable → not swept. (Cross-node stateless-reader pins = B3/B11.)
2. **In-flight inserts** — uploaded-but-not-yet-referenced blobs are young+unreachable ⇒ spared by `grace`; if a writer references a swept blob it re-uploads via `putIfAbsent` (it has the bytes). Abandoned inserts age out.
3. **Mutation carry-forward** — source stays live through the mutation; the new part commits referencing the same blobs before the source goes outdated → continuous reachability, no window.
4. **Genuine orphans** (merged-away / dropped / replaced) — collected after grace; shared blobs stay reachable via whatever still references them.

## 7. Scaling (millions of parts) {#scaling}

Four pressure points and their mitigations, all designed in:

1. **GC scan cost** — solved by the delta-driven `BlobRefIndex` (no per-pass full `LIST`); full reachability is the rare, prefix-sharded reconciliation path only.
2. **Memory** — the refs map (name→part_id, ~150 B/part) is ≤ the `data_parts` set ClickHouse already holds. The only O(M-distinct-blobs) structure is the `BlobRefIndex`; M1 keeps it in memory, the scale impl (B9) persists it to RocksDB and caches a hot subset. The metadata storage is lazy (LRU footers).
3. **Write PUTs** — C blobs + 1 footer + 1 ref per part (≈ today's per-file count + 2); `putIfAbsent` dedup reduces blob PUTs; merges consolidate tiny parts (a workload concern, not layout-induced).
4. **Read GETs** — footer + small-file GETs per cold open, amortized by `FileCache`; **B10** (pack small files → one GET) is the read-cost-at-scale optimization.

## 8. Concurrency, multi-writer, and replication readiness {#concurrency-and-replication}

**Invariant: one pool (disk root) = exactly one GC coordinator.**

- **Multiple tables, same server, same disk** — fine; one metadata-storage instance, one GC, one pool (intended cross-table-dedup case).
- **Pool coordination is a *disk-level* property, orthogonal to table replication.** A CAS disk is configured `coordination = none | keeper`:
  - **`none`** (single mounter) — the disk holds an S3 ownership lease and **fails closed** if it detects another live mounter. M1 default.
  - **`keeper`** (multi-mounter — *including but not limited to replication*) — the disk joins a Keeper coordination path for **leader-elected GC + the GC lock**; refs are published through the **Keeper `RefCatalog`**, whose change-watches give the single-leader GC its **delta feed** (so even a *non-replicated* table on a multi-mounted pool stays delta-driven and safe, no full `LIST`, no replication `/log` required).
- **Self-check (first-class in M1).** The pool carries a `_pool_meta` record (pool format version, `coordination` mode + Keeper path, owner/leader lease). Every mounter validates its config against `_pool_meta` at startup and periodically, and detects other live mounters; on any conflict/mismatch — two `none` mounters, `none`-vs-`keeper`, divergent Keeper paths, incompatible format version — it **fails closed**. This catches the misconfigurations that would otherwise corrupt the shared pool.
- **Independent processes sharing one pool** (two servers on one bucket, *or* replicas of one table) are the **same problem**, solved by `coordination = keeper`: single-leader GC + lock, mark over the **union of all `store/<serverid>/…/refs`**, re-validate-at-delete. **The layout does not change** — mounters just share the pool; only the coordination layer differs, behind the `RefCatalog` and GC-coordination seams.

**Replication readiness (B1):** content pool unchanged; replication is a *consumer* of `keeper` pool coordination, adding only per-replica `/parts` semantics + the `/log`. `RefCatalog` swaps S3→Keeper; GC-coordination swaps in-process→Keeper leader+lock; mark becomes union-over-serverids. No layout or GC-logic rework.

## 9. Configuration / opt-in {#configuration}

A new `metadata_type = content_addressed` on an object-storage disk (the `plain_rewritable` precedent). Users add that disk to a storage policy and point a normal non-replicated `MergeTree` at it — no engine, no DDL change. Disk/server settings: `coordination` (`none` | `keeper`) + `coordination_keeper_path`, `grace`, `gc_period`, `BlobRefIndex` impl, scratch volume.

**Fail-closed feature gate.** M1 must *reject* (at `CREATE`/`ATTACH`) what it does not yet implement, rather than silently mishandle it: tables with **projections** or **patch parts / lightweight delete** (B5), and `ALTER … FREEZE` if its DDL is deferred (B4). It also fails closed on an unrecognized pool/footer/ref **format version** and on the pool self-check (§8). Features are enabled incrementally per the backlog.

## 10. Error handling and failure modes {#error-handling}

- **Crash after upload, before ref publish** → unreferenced objects, aged out by GC. Benign.
- **Crash mid-build** → local scratch discarded; any partial uploads age out.
- **`BlobRefIndex` drift / loss** → it is a cache; rebuilt by prefix-sharded reconciliation; `grace` + re-validate-at-delete bound transient drift.
- **Uncoordinated shared pool** → `_pool_owner` fail-closed.
- **Hash collision** → `(checksum, size)` guard, verified on read.

## 11. Testing strategy {#testing}

1. **Algorithmic oracle** — port the PoC's scenarios (dedup, carry-forward, covering, drop, GC grace, reader survival, detach/attach) as unit tests over the production types.
2. **Functional (local-vs-CAS oracle)** — a `content_addressed` disk in `tests/config`; run a representative slice of existing stateless `MergeTree` tests (insert/merge/mutate/drop/detach-attach/restart-reload) against it, asserting identical query results to a normal disk.
3. **GC** — drop/mutate then assert exact blob reclamation; assert shared/carry-forward/in-flight blobs survive; a crash-then-reconcile test.
4. **Fault injection** — around the abandoned-insert and carry-forward windows.
5. **Scale smoke** — N×C parts; assert GC stays delta-bounded (no full `LIST`) and the `_pool_owner` guard fail-closes a second process.

## 12. Residual risks / open questions {#residual-risks}

- Exact hook for **build-local-then-upload** in the write path (per-file spill vs temp-part-on-local-volume) — to settle in the implementation plan.
- `grace` default and clock/visibility-skew margin.
- Whether `ALTER … FREEZE` writes frozen refs in M1 or defers entirely (namespace + GC-root reserved either way).
- Manifest/footer canonicalization determinism (golden tests) — B6.
- **Cross-producer dedup is settings-dependent** — identical content dedups only when compression / `index_granularity(_bytes)` / sparse-serialization settings match across producers; mismatch → no dedup (more storage), **never incorrect**. A quality caveat, not a correctness risk.
- Whether **`coordination = keeper`** ships in M1 or as the immediate next milestone (the self-check + `coordination = none` ship in M1 regardless).
- Migration / mixed-version rollout — B13.

## 13. Deferred backlog {#deferred-backlog}

The authoritative list of deferred work, each with its plug-in point (the dead-end proof), lives in `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (B1–B12 + the scale/concurrency invariants). It is maintained alongside this spec and the implementation plan.
