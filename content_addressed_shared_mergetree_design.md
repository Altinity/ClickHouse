# Content-Addressed Shared Storage for MergeTree ("Git for MergeTree") — v3

**Status:** design draft for discussion. v3 folds in a second, code-verified adversarial review. The biggest change is honesty about *removal*: "commit = create one covering ref" was an additive-only story, and removal needs a real primitive (an extended empty tombstone part). v3 also fixes the stateless-reader GC fence, downgrades single-producer from a correctness requirement to an optimization, corrects FREEZE (must materialize bytes), softens the "one class / one GET" claims, and adds patch parts to the GC model.

## 0. Essence

Store every part as **content-addressed immutable objects** — column blobs keyed by their existing `checksums.txt` `cityHash128`, and a per-part *manifest* keyed by its own hash. Liveness is the **existing** ClickHouse machinery, unchanged: the active set is derived from **part names + the covering rule** (`ActiveDataPartSet`), each replica keeps its own `/parts/<name>` ref, and refs are released by the existing outdated-parts lifecycle. Reclaim space with **reachability GC** over the union of all replicas' refs → manifests → blobs, gated by that same lifecycle. No commit journal, no per-blob locks, no per-blob refcounts, no hardlinks.

| Git | This design |
|---|---|
| `blob` (keyed by SHA) | `blobs/<hash>` — one column stream, key = the file's existing `checksums.txt` `cityHash128` (+ size) |
| `tree` / `commit` (keyed by SHA) | `manifests/<hash>` — the part manifest (blob-hash list + inline service files + footer) |
| `refs/heads/*` | the **existing** `/parts/<name>` refs (Keeper, per-replica) / a tiny listable `refs/<name>` set in S3 (non-replicated) |
| naming → which commit is current | **part name + `ActiveDataPartSet` covering** — already MVCC, no log needed |
| `git gc` | reachability over the union of refs, gated by the existing outdated-parts lifecycle |

## 1. Why content-addressed, and what it does / does not buy

- **Cross-replica dedup is real, and it follows from single-producer + reference, not from byte-reproducibility.** ClickHouse parts are *not* bit-reproducible across independent rebuilds (compression block boundaries, vertical-merge interleaving, default-codec choice depending on per-replica `getTotalActiveSizeInBytes`, TTL `now()`). So content addressing does not dedup *independently recomputed* parts. It dedups because a part is produced by **one** replica and *referenced* by the others. The single-producer property already exists (see §4 MERGE); v3 treats keeping it as the recommended *operating mode and optimization*, not a correctness precondition (see §4 for why divergence is benign here).
- **Idempotent writes, free intra-part carry-forward, cross-table share — all by reference.** A mutated part's manifest references the unchanged columns' existing blob hashes; this deletes the `MutateTask` `checksums.txt`-copy hack, the `HardlinkedFiles` plumbing, and `getParentLockedBlobs`, replacing them with reachability.
- **Self-describing per object; immutable keys ⇒ the local cache never goes stale.**
- **What it does NOT buy:** dedup of independently-recomputed bytes; dedup across tables with *different* `<compression>` / `ratio_of_defaults_for_sparse_serialization` / `index_granularity(_bytes)` (the compressed bytes differ ⇒ different hashes); any benefit for **Compact** parts on mutation (a compact part is one blob — it is fully rewritten, no carry-forward). Carry-forward and cross-table dedup apply to **Wide** parts (default `min_bytes_for_wide_part` = 1 GiB on remote disks), i.e. the large parts where it matters most.

## 2. Object layout and keys

```
<catalog_root>/                       # one bucket/prefix == exactly one catalog (one table or coordinator)
  blobs/<h0>/<h1>/<full_hash>          # one checksummed column stream; key = checksums.txt cityHash128 (+ size guard)
  manifests/<h0>/<h1>/<full_hash>      # one part manifest (small object)
  refs/<part_name>                     # tiny: -> manifest_hash  (LISTABLE; non-replicated truth + replicated DR)
```

No table/replica/partition directories, no `__<gen>` token (content addressing is idempotent), no journal/checkpoint.

**Blob keys — checksummed files only.** `IMergeTreeDataPart::getFileNamesWithoutChecksums` excludes six service files (`checksums.txt`, `columns.txt`, `columns_substreams.txt`, `default_compression_codec.txt`, `metadata_version.txt`, `txn_version.txt`). Only the **checksummed** files (`.bin`, marks, `primary.idx`, `minmax_*.idx`, `count.txt`, `partition.dat`, `ttl.txt`, `serialization.json`, statistics) are content-addressed blobs, keyed by the **on-disk** `file_hash` (`plain_hashing`, not `uncompressed_hash`) **plus the file size** as a collision guard (CityHash128 is non-cryptographic; on INSERT the bytes are attacker-influenceable — verify size on read).

**Manifest.** A small object containing:
- the **footer**: for each logical file, `(blob_hash | inline_offset, length, checksum)`, sorted by logical file name, plus a **granularity summary** (so adaptive granularity does not require a separate marks GET to open the part);
- the **six non-checksummed service files inline** (their bytes embedded, hashed as part of the manifest's canonical body);
- the **small checksummed open-time files inline too**: `primary.idx`, `partition.dat`, `minmax_*.idx`, `count.txt`, `ttl.txt`, `serialization.json`. These are tiny, are touched on every part open, and embedding them is what lets a single manifest GET answer **all** part-open metadata and index needs (see §2 "Reading" and problem #7). `.bin` and marks stay as **separate whole content-addressed blobs** (they are large and selectively read);
- nested **projection manifests** (a `.proj` checksums entry is a SipHash hash-of-hashes, not a byte address — model each projection as its own nested manifest, parent references the child manifest hash).

**Excluded from the hashed manifest body** (else identical parts get different hashes / mutable data lands in an immutable object): `MergeTreePartInfo` (the part name / block numbers — lives in the ref), `uuid.txt` (random under `assign_part_uuids`), `txn_version.txt` and `metadata_version.txt` (mutated in place after commit — see §3).

**Reading — one GET for all part-open metadata+index, sliced in memory; no `FileCache` change.** The manifest is fetched as a **whole object** (cached whole by its own key); the footer and the inline files above are sliced from it **in RAM** by the new part storage. Because `primary.idx` is inline, the open cost is one GET even though `primary_key_lazy_load=true` (default) defers the actual index materialization. Large `.bin` and marks stay as **separate whole objects** read through the existing stack. This deliberately avoids sub-object slicing (`StoredObject` has no offset field; `readObject` reads from byte 0) — so `FileCache`/`CachedObjectStorage` are genuinely untouched. This is the single property that satisfies **both** problem #7 (no tiny-GET storm) **and** "FileCache unchanged".

## 3. Refs, commit, and per-part mutable state

The active set is **derived from part names + covering** (`ActiveDataPartSet`), exactly as today. A ref carries `{ManifestHash} ∪ {the existing ReplicatedMergeTreePartHeader: columns_hash + MinimalisticDataPartChecksums} ∪ {mutable per-part fields}`.

- **Keep the existing `/parts/<name>` header, append `manifest_hash`.** The header is read by *other* replicas (`checkPartChecksumsAndAddCommitOps`, `ReplicatedMergeTreePartCheckThread`, RESTORE) for cross-replica divergence detection (KILL-MUTATION skew). We do **not** shrink it; we add the manifest pointer.
- **Mutable per-part state lives in the ref, never in the manifest:** `txn_version.txt` (CSN appended after commit, removal TID on drop) and `metadata_version.txt` (rewritten in place by metadata-only ALTER). These change over a part's life while its content hash is fixed — so they cannot be in the immutable manifest. Transactions and metadata-only ALTER are explicitly in scope.

### Commit = create ref(s) — but "covering" needs a removal primitive

The clean invariant is **commit = create ref(s)**; supersession of the old active set is by *covering*, not by deleting source refs in the commit. But "create one covering ref" only literally holds for **additive** supersession. Removal is different and needs its own primitive. Split it:

**(a) Additive supersession — INSERT, `MERGE_PARTS`, `MUTATE_PART`.** Genuinely "create exactly one covering ref" pointing at a real (data-bearing) part. `MergeTreePartInfo::contains` covers both a level bump (merge of `[a,b]` into one higher-level part) and a mutation bump (`MUTATE_PART` raises the mutation version), and `getActiveContainingPart` finds that covering part among `DataPartState::Active` parts and marks the in-range parts inactive. Nothing to remove.

**(b) Removal supersession — DROP PART, DROP PARTITION / `DROP_RANGE`, the source-side removal of REPLACE / MOVE PARTITION FROM.** This is **not** covering-expressible by a data-less marker as the code stands today:
- `ReplicatedMergeTreeLogEntryData::getVirtualPartNames` returns `{}` for `DROP_PART` (deliberately — "we have very weak guarantees for DROP PART"); it returns `{new_part_name}` for `DROP_RANGE`.
- `getActiveContainingPart` only walks **real** `DataPartState::Active` parts. A pure logical "covering marker" that is not itself an Active part in `data_parts_by_state_and_info` covers nothing.

So a removal cannot be a no-data ref that "just covers." **The fix adopted in v3:** *extend the empty tombstone part that already exists.* `MergeTreeData::dropPartImpl` (around `MergeTreeData.cpp:5692`) already constructs an **empty covering part** over the drop range — but today it is created **local, Outdated, uncommitted** ("We don't need to commit it to zk, and don't even need to activate it"), purely to stop a restarted server from rejecting unexpected parts. v3 promotes that exact part into a **persisted, Active, covering ref**: an **empty (data-less, zero blobs) tombstone manifest** whose `MergeTreePartInfo` covers the range. Once it is Active, `getActiveContainingPart` returns it for every in-range part and marks them inactive — *covering now works for removal too*, with the same `ActiveDataPartSet` machinery and no new "delete" verb.

This keeps **commit = create ref(s)** uniform:
- additive → create one **real** part ref;
- removal → create one **empty covering tombstone** ref (no blobs to upload), made Active and persisted.

Caveats, each load-bearing:
- **DROP PARTITION / DROP_RANGE is clean:** one max-level empty covering ref over the whole range (the range starts at block 0, so no intersecting-parts hazard — the same condition the existing code checks with `range_in_the_middle`).
- **DROP PART of a single leaf needs care.** `dropPartImpl` deliberately *avoids* the fake-drop level for a part in the middle, precisely because a concurrent merge may produce a **real** covering part that carries the data — DROP PART then "deletes nothing and the part is merged into a bigger part" (the documented weak guarantee). The tombstone must **reconcile with that race**: if a real covering part wins, the data legitimately survives under a new name, and the tombstone must not strand or double-cover. The persisted tombstone for a mid-range single part is therefore conditional/reconciled, not unconditional.
- **REPLACE / MOVE PARTITION FROM is intrinsically multi-ref:** N new real refs (the replacement parts) **plus** one covering tombstone over the replaced source range. This is still "create refs" — there is no separate delete primitive — just more than one ref in the commit.

For **replicated** tables the existing `/log` entries (`DROP_RANGE`, `DROP_PART`, `REPLACE_RANGE`) are kept **verbatim**; consensus, queue ordering, covering, and the weak-DROP-PART semantics are unchanged. For the non-replicated `LocalCatalog`, a DROP **commits a persisted tombstone covering ref** — it is explicitly *not* a "create-only-of-real-parts" model.

### Commit substrate

Sources are *not* removed in the commit — covering (additive part or tombstone) makes them inactive; their refs are released later by the existing lifecycle (§5).
- **Replicated → the existing Keeper multi, unchanged in shape** (`/blocks/<dedup>` + `/parts/<name>{header, manifest_hash}` + `/log GET_PART` / `DROP_RANGE` / `DROP_PART` / `REPLACE_RANGE`). Consensus, queue, covering, block numbers, insert-dedup — all unchanged. There is **no commit journal** and therefore no S3↔Keeper split commit and no follower-ordering proof.
- **Non-replicated → one `PutObject`** of `refs/<part_name> -> manifest_hash` (idempotent; `If-None-Match:*` only to avoid clobber on retry). For a DROP this PUT writes the **tombstone** ref. Active set = `LIST refs/` + covering. No Keeper, no refcounts, no ordering.

**Portability:** non-replicated needs only create-if-absent (not CAS-overwrite, since there is no pointer to swap — covering handles supersession). Add a startup **probe** that the backend *enforces* the header; fail-close. Replicated does not use S3 conditional writes at all.

## 4. Flows

**INSERT.** `MergedBlockOutputStream` produces the part into a **local staging dir** (packing is local-disk cost; S3 writes stay 1×). Upload checksummed files as `blobs/<hash>` (create-if-absent, idempotent), pack + upload `manifests/<hash>`, then one commit (Keeper multi / one PUT). Durability = objects in S3 (strongly consistent) + the ref committed.

**READ.** Active set from in-memory `MergeTreeData` (kept fresh by `/log` for replicated; `LIST refs/` + covering for non-replicated). Resolve `ref.manifest_hash` → GET manifest (whole) → footer + inline metadata/index → blob reads via `DiskObjectStorage → CachedObjectStorage(FileCache) → IObjectStorage`. One GET for all part-open metadata+index.

**REPLICATE.** No `DataPartsExchange` byte copy: a replica seeing `GET_PART` creates its own `/parts/<name>` ref pointing at the shared manifest (the producer already wrote the blobs). `DataPartsExchange` retained only for hot-tier population.

**MERGE / MUTATE — single producer is the recommended mode, divergence is benign.** Assignment uses the existing deterministic single-replica path (`execute_merges_on_single_replica_time_threshold` / `ReplicatedMergeTreeMergeStrategyPicker`), enabled for this engine; others reference the winner. A replica that *did* compute a merge whose part name **already exists** in the catalog (another replica won) can **adopt by reference** — point its ref at the winner's `manifest_hash` instead of recomputing/uploading — and can check the catalog *before* uploading blobs to avoid wasted PUTs. A partial-column mutation's new manifest references the unchanged columns' existing blob hashes (carry-forward by reference). Compact parts are fully rewritten (no carry-forward).

**Single-producer / divergence is an OPTIMIZATION, not a correctness requirement** (this is the v3 downgrade from v2's "must reintroduce a `/merge_claims` lease + re-key divergence on `manifest_hash`"). Code-verified rationale:

- The commit-time cross-replica check `checkPartChecksumsAndCommit` → `checkEqual(..., /*check_uncompressed_hash_in_compressed_files=*/ true, ...)` (`StorageReplicatedMergeTree.cpp:2243`) runs in **tolerant mode**: `MinimalisticDataPartChecksums::checkEqualImpl` with `check_uncompressed_hash_in_compressed_files=true` compares `uncompressed_hash_of_compressed_files` (`MergeTreeDataPartChecksum.cpp:474-481`). Two replicas with **identical uncompressed content but different compressed bytes** therefore **pass** the check.
- In the content-addressed model that yields, at worst, **two manifests + two blob sets for one part name** (each replica references its own). This is **not a correctness bug**: reachability, refcounts, and query results are all correct, and both blob sets are GC'd when the part is later superseded. It is a **P1 dedup-MISS, not P0** — and the v2 phrasing "lives forever" was wrong: the duplicate lives only for the part's lifetime.
- In a **homogeneous cluster with deterministic compression** (same server version, fixed codec, same `max_compress_block_size`) the bytes are identical → identical blob hash → dedup works with **no check at all**.
- The mitigation is therefore minimal and already in-tree: **enable the existing single-replica-merge picker** so one producer is the norm. `adopt-by-reference` and any `/merge_claims` lease become **optional cost optimizations**, not correctness requirements. Note `shouldMergeOnSingleReplica` reads only `execute_merges_on_single_replica_time_threshold`; the `remote_fs_execute_merges_on_single_replica_time_threshold` variant is set in the picker's threshold init but never consumed by `shouldMergeOnSingleReplica` — enabling the feature for this engine should use the consumed setting.
- **Genuine divergence is still caught.** The strict branch of `checkEqualImpl` (the `check_uncompressed_hash_in_compressed_files=false` path comparing `hash_of_all_files`) and **non-compressed files** (which always compare the on-disk `file_hash` in `MergeTreeDataPartChecksum::checkEqual`, `MergeTreeDataPartChecksum.cpp:54-63`) still fire on real corruption and on a compression-library change. We lose no real-divergence detection by relying on the tolerant commit check plus content addressing.

**DETACH / ATTACH / FREEZE / FETCH** (first-class; the `disable_{detach,fetch,freeze}_partition_for_zero_copy_replication` guards are removed for this engine):
- **DETACH** = move the ref to a `detached` namespace (still a reachability root). No byte movement.
- **ATTACH** (intra-table) = read an existing manifest → create one ref.
- **ATTACH / MOVE PARTITION FROM** (cross-table) = **copy** the referenced objects into the destination catalog's pool, then create the dest ref (a fresh block number per `lock.getNumber()`), **and** a source-side covering tombstone for MOVE. This is still strictly better than today (the op is *enabled and correct*, not disabled), and it sidesteps cross-catalog GC (a source DROP can never strand a destination ref). True zero-copy cross-table sharing would require one shared catalog/coordinator and is explicitly out of scope (see §5 scoping).
- **FREEZE = materialize real bytes into `shadow/`.** `freeze`, `freezeRemote`, and `clonePart` are **independent `IDataPartStorage` virtuals** (`IDataPartStorage.h` ~270-296), **not** built on top of `createHardLinkFrom`. `freeze` writes to the local `shadow/` directory whose documented contract is *filesystem-readable hardlinks consumed by external backup pipelines*. A reference-only FREEZE (just emitting a ref-set object) would silently break every filesystem-level backup. **Therefore FREEZE on this engine MUST materialize real bytes** — GET/copy the referenced blobs into `shadow/` as ordinary files. (A self-describing `snapshots/<name>` ref-set can still exist as an *additional* in-pool reachability root for restore-by-reference, but it does not replace the `shadow/` byte materialization.) This moves FREEZE/clone from "unchanged" to "reworked at the `IDataPartStorage` seam" — see §8/§10.

**DROP PARTITION / DROP PART** = commit a covering tombstone (§3): one max-level empty ref for a partition/range, a reconciled empty ref for a mid-range single part. Covered refs go inactive by covering; their blobs are released and swept by the lifecycle below.

## 5. GC — reuse the existing outdated-parts lifecycle, do not invent a parallel one

### Source of truth for "removable"

A part stops being a *root* the instant a covering part (real or tombstone) is committed. Its `/parts/<name>` ref is **released by the existing path** — `clearOldPartsAndRemoveFromZK` / `grabOldParts`, gated by `old_parts_lifetime` (default 480 s) **and** `isSharedPtrUnique` (no in-flight SELECT on a ref-holding replica). We reuse that signal verbatim; there is no separate tombstone with separate, mis-timed accounting.

### Reader fence — `isSharedPtrUnique` only fences ref-holding replicas

`isSharedPtrUnique` is a **per-process** refcount (`use_count()==1`). It is a valid cross-node fence **only for replicas that hold a `/parts` ref** for the part being read: such a replica keeps the `DataPartPtr` alive while a SELECT touches it, the ref stays in Keeper, and the GC union sees it. This is why, for ref-holding replicas, the grace window need **not** be sized to the longest SELECT — the v2 claim is correct *for that case*.

It does **not** cover the §7 stateless / ref-less reader. A stateless compute node that attaches by reading the catalog **registers no `/parts` ref**, so it is **invisible to the GC MARK union**. The owning replicas can release their refs after `old_parts_lifetime` (480 s) **mid-SELECT** on the stateless node → the manifest/blobs become unreachable → SWEEP deletes the objects → the read fails. The v2 "grace need not cover the longest SELECT" is therefore **wrong for ref-less readers**.

**Fix:** a stateless reader creates **ephemeral state** that pins its query snapshot and is **included in the GC MARK union**:
- An **ephemeral Keeper node** (or an equivalent object) created at query start, naming the parts (or the snapshot) it reads, auto-released on Keeper session end / crash (so a crashed reader **cannot leak** the pin).
- **Grain options:** one ephemeral pin per part for normal queries (precise, more nodes), or a **single per-snapshot pin** (naming the `log_pointer` / oldest part of the snapshot) for huge scans (cheap, coarse).
- The alternative for deployments without a coordinator is to size grace ≥ the longest ref-less SELECT — but the ephemeral pin is preferred because it is self-bounding.

Two related leak notes:
- A **crashed-but-not-dropped replica** retains its `/parts` refs (they outlive the dead process), which **pins blobs forever** and is a genuine leak source. This needs a **lost-replica timeout** (treat a long-dead replica's refs as releasable) — distinct from the ephemeral-reader pin, which self-releases on session loss.
- `DROP REPLICA` already removes the replica's `/parts` subtree, so it correctly drops that replica's contribution to the MARK union.

### Safety invariant

> A blob/manifest is deletable **iff** it is referenced by **no** `/parts` ref of **any** replica **and** by **no** ephemeral reader pin (transitive closure over the union of all replicas' refs + all reader pins → manifests → blobs), **and** the existing lifecycle has released every such ref. "Referenced by any replica" subsumes the cross-replica MAX: a blob lives until the **last** replica drops its ref.

Parts are global; refs are per-replica; GC operates on the **union** of refs **and** ephemeral reader pins. Reachability is transitive (a manifest may reference blobs of other parts — carry-forward, and patch parts — §7) and includes `detached/*`, `snapshots/*` (FREEZE ref-sets), and lag-retention.

### Protocol

1. **Single coordinator** per catalog via an ephemeral Keeper lock; **self-healing** — any replica that sees the lock stale for N intervals re-runs a pass. A watchdog metric + hard alert fire if no pass completes within a bound (a stalled coordinator must not silently leak).
2. **MARK:** transitive reachable set over the union of refs **+ ephemeral reader pins + tombstone-reconciled removals**; record the catalog epoch (`/log` version).
3. **SWEEP:** delete a candidate **iff** not in the mark set **and** the lifecycle has released all its refs **and** — re-validated under the GC lock at catalog epoch ≥ MARK — still unreferenced. Batched `DeleteObjects`, rate-limited.
4. **Dedup-sharpened old-blob race** (a new part references an old, becoming-unreferenced blob — identical-content INSERT, carry-forward, restore): the writer guarantees referenced blobs exist at commit (it has the bytes for INSERT/carry-forward; cross-table ATTACH **copies**, so it never references a foreign-pool blob). For any reference the writer cannot re-materialize, fail-close: HEAD after taking the GC lock, fail the op if a blob is gone — never commit a dangling ref.
5. **Steady-state GC is driven by lifecycle ref-removals (a delta), not a full `LIST`.** Full `LIST blobs/` is a rare audit/DR backstop. Enumerate **every** ref-removal site (`clearOldPartsAndRemoveFromZK`, `REPLACE_RANGE`/`DROP_RANGE` removal, `forcefullyRemoveBrokenOutdatedPart…`, broken-part fetch, DROP REPLICA subtree deletion, lost-replica-timeout reaping) and prove each feeds GC — a missed site leaks (P0 invariant, with a test).
6. **Compliance fast-path:** `DROP … SYNC` deletes immediately under the GC lock after confirming no reachable manifest references the objects (operator asserts no in-flight reader), bypassing the lifecycle delay.

### Scoping (hard rule)

**One bucket/prefix == one catalog** (one `zookeeper_path`, all its replicas). Cross-table dedup only within a shared coordinator; **cross-cluster bucket sharing is forbidden** — per-catalog reachability would delete blobs live in another catalog. This is why cross-table ATTACH copies (§4).

### Crash recovery / DR

- Crash after upload, before commit → unreferenced object, swept by the lifecycle/backstop. Benign.
- Crash mid-MPU → `AbortIncompleteMultipartUpload` lifecycle rule.
- Crash after commit, before reply → durable; retry is a no-op (`/blocks/<dedup>` or the ref already exists).
- **Keeper subtree lost (replicated)** → DR rebuild replays the **entire committed `/log` forward through `ActiveDataPartSet`** (applying `MERGE_PARTS` / `MUTATE_PART` / `DROP_RANGE` / `REPLACE_RANGE` supersession) and recreates `/parts/*` **only for parts active in the final covering set** — never a create-entry later superseded (no phantom resurrection). A create whose blobs are already gone (post-supersession) is dropped, not resurrected — fail-close. **`DROP_PART` removals must be folded explicitly**, not via covering: because `getVirtualPartNames` returns `{}` for `DROP_PART` (the documented weak guarantee — a concurrent merge may have absorbed the data into a real part), the replay must apply DROP PART as an explicit removal pass and reconcile it with any winning merge, rather than assuming a covering virtual part exists. The S3 `refs/` set + covering (including persisted tombstones) is the equivalent rebuild source where `/log` is also gone.

## 6. Replicated and non-replicated share one machine

One `ICatalog` seam: `getActiveParts` (= refs + covering, tombstones included), `commit(ref…)`, `resolve(name)→{header, manifest_hash, mutable fields}`.
- **`KeeperCatalog`:** commit = the existing Keeper multi (`GET_PART` / `DROP_RANGE` / `DROP_PART` / `REPLACE_RANGE` unchanged); active set kept fresh by `/log`; GC over the union of replicas' refs + reader pins via the existing lifecycle. No refcounts (reachability).
- **`LocalCatalog`:** no Keeper, no refcounts. commit = one `PutObject` of `refs/<name>` (a real part ref, or a **persisted covering tombstone** for a DROP); active set = `LIST refs/` + covering; GC = single-process reachability over `refs/`. Pure non-replicated is single-**process**; external stateless readers need a coordinator (then it is `KeeperCatalog`, with ephemeral reader pins).

Identical layout, manifest format, covering, and GC. Only the commit/listing substrate differs.

## 7. Multidisk, caching, stateless compute, patch parts

- **Multidisk:** unchanged `StoragePolicy`/`IVolume`; this is one per-disk layout among others. Hot-local + cold-CA-S3 coexist as today's TTL tiers; `TTL MOVE` = re-pack to destination + ref-flip.
- **Caching:** **no new code** — `CachedObjectStorage` + `FileCache`, immutable keys ⇒ no invalidation (manifest cached whole, `.bin` and marks whole).
- **Write-through / double-write:** a local volume as write-through cache; immutable objects are interchangeable local/S3 (no divergence).
- **Stateless compute:** a fresh node attaches by reading the catalog; holds nothing authoritative. It **must register an ephemeral reader pin** for the duration of each query so the GC MARK union sees its snapshot (§5 reader fence) — otherwise an owning replica's lifecycle release can delete objects mid-scan.

### Patch parts / lightweight delete

Patch parts (lightweight `DELETE`/`UPDATE` deltas) live in a **separate partition namespace**, not in the base covering set:
- `ActiveDataPartSet` filters them out of the base active set (it skips entries where `kv.first.isPatch()`), and `MergeTreePartInfo::isPatch` (`kind == Kind::Patch`) tags them. They are tracked as their **own** active set and **applied on top of** the base covering set at read time, not merged into it.
- In this design a patch is its **own ref class**: a **patch manifest** that **references base-part data** (the columns it patches) plus its own delta blobs. A patch ref is a **first-class reachability root**, exactly like a regular part ref.
- **GC model:** while a patch is active, its ref keeps alive **both** its own delta blobs **and** the **base blobs it references** (transitive reachability through the patch manifest). A base part therefore cannot have its blobs swept while a live patch references them, even if the base part itself were otherwise superseded — the union-of-roots already expresses this, provided the MARK walk treats patch manifests as roots and follows their references into the base.
- Enumerate patch lifecycle into the ref-removal sites (§5.5): when a patch is materialized into the base (merge), the patch ref is released like any other ref, and its uniquely-owned delta blobs become sweepable.

If patch-on-content-addressed-base is not fully modeled by M-whenever it lands, it stays in residual risks (§12) until proven.

## 8. Layering — a single new `IDataPartStorage`, but it overrides several methods

This is the answer to "we seem to change part layout AND paths AND disk handling at once." We add **one new storage class**, but in the interest of honesty: it **overrides several methods**, not one. Map to the existing stack:

| Concern | Layer | Change |
|---|---|---|
| Part type (Wide/Compact) | `IMergeTreeDataPart` | **none** (no `*_cas` types) |
| Packing + content-addressed keys | `IDataPartStorage` | **one new class, ~10 overrides** |
| path→object indirection ("local link") | `IMetadataStorage` | replaced by the footer (bypassed) |
| object get/put/list | `IObjectStorage` | **none** (+ a capability probe) |
| active set / commit / tombstones | `MergeTreeData`/`StorageReplicatedMergeTree` | minimal: source parts from refs, publish = create-ref(s) |
| metadata-version in place | part layer (`writeMetadataVersion`) | redirect to a ref rewrite |
| GC | new background component | replaces the deleted zero-copy `Replication/` GC |

The pivot remains: **`PackedObjectDataPartStorage` (new `IDataPartStorage`) reinterprets `createHardLinkFrom` as "share the same blob hash."** But that is the pivot, not the whole story. The seam class also overrides, with content-addressed semantics, roughly: `freeze` and `freezeRemote` (materialize real bytes into `shadow/`, §4), `clonePart`, `copyFileFrom`, `createProjection` (nested manifest), `replaceFile`, `moveFile`, `removeFile`, and `iterate` (it must present the **logical** file list reconstructed from the footer, not raw objects). In addition, a **part-layer redirection** is needed for `writeMetadataVersion` (today an in-place rewrite of `metadata_version.txt`): on this storage the metadata version lives in the **ref**, so an in-place write must instead **rewrite the ref**.

What genuinely stays unchanged: the part **types** (`Wide`/`Compact`) and `IObjectStorage`. And `MutateTask`'s hardlink calls become **carry-forward-by-reference** on this storage for free (no `MutateTask` rewrite), because the hardlink seam routes through the new class.

Independently testable: M1 = round-trip a part through `PackedObjectDataPartStorage` (local-vs-CA oracle), no Keeper/replication/GC.

## 9. Problem → solution map

| # | Problem | Solution |
|---|---|---|
| 1 | Copy per replica | Replicas reference the same content-addressed blobs (single-producer mode); carry-forward + cross-table by reference. |
| 2 | Zero-copy locks/refcounts; ops disabled | Refcount/lock subsystem deleted; GC = reachability over refs + reader pins via the existing lifecycle. DETACH/ATTACH/FETCH/FREEZE are first-class (FREEZE materializes bytes). |
| 3 | Non-self-describing layout | Per-object self-describing (footer + content hashes). Liveness via names+covering (+ persisted tombstones). |
| 4 | Local↔S3 divergence | Liveness = existing covering/lifecycle; S3 `refs/`+covering rebuilds Keeper. |
| 5 | Awkward share-nothing-over-shared | Consensus + active-set machinery unchanged; only layout + GC change. No journal, no S3↔Keeper transaction. |
| 6 | Per-table root | One catalog root; nodes attach by reading the catalog. |
| 7 | Tiny GET/PUT storm | One packed manifest per part; one GET for all part-open metadata+index (index/minmax/count/serialization/granularity inline). |

## 10. Reuse vs. rework

**Reused unchanged:** `ReplicatedMergeTreeQueue`, `ReplicatedMergeTreeLogEntry` (incl. `DROP_RANGE`/`DROP_PART`/`REPLACE_RANGE` verbatim), `ActiveDataPartSet`, `MergeTreePartInfo`; `/parts` header + `commitPart` shape (append `manifest_hash`); `ReplicatedMergeTreeMergeStrategyPicker` + single-replica-merge + commit-time checksum reconcile (single-producer is now an *optimization* — see §4); the outdated-parts lifecycle (`grabOldParts`/`clearOldPartsAndRemoveFromZK`); `MutateTask`/`cloneAndLoadDataPart` (hardlink → reference, via the storage); `MergedBlockOutputStream`; `FileCache`/`CachedObjectStorage`; `DiskObjectStorage`/`IObjectStorage`; `StoragePolicy`/`IVolume`; the `checksums.txt` hashes; the part **types** (`Wide`/`Compact`).

**New, bounded:** `PackedObjectDataPartStorage` (the pivot, ~10 overrides); `ManifestWriter`/`Reader` (+ canonicalization, projection nesting, inline-index packing); `ICatalog` (`KeeperCatalog`/`LocalCatalog`); content-addressed key gen; **persisted covering-tombstone commit** (extending the existing empty-part construction in `dropPartImpl`); `ReachabilityGC` (mark/sweep gated by the lifecycle, with the **ephemeral reader-pin** union and **lost-replica-timeout** reaping — a net deletion of the zero-copy `Replication/` code); `IObjectStorage` capability probe.

**Honest rework:**
- The publish point in `commitPart` / the log-entry tasks changes from directory-rename to ref-create (bounded, above `IMetadataStorage`).
- **FREEZE / `freeze` / `freezeRemote` / `clonePart` are reworked at the `IDataPartStorage` seam** — they must materialize real bytes into `shadow/` (§4), not emit references. (v2 listed FREEZE as "unchanged"; that was wrong.)
- `writeMetadataVersion` redirects from an in-place file write to a ref rewrite.
- The merge adopt-by-reference logic (optional optimization).
- DROP/REPLACE/MOVE removal becomes a persisted-tombstone commit, with DROP-PART mid-range race reconciliation.
- Cross-replica divergence detection is preserved by keeping the header; in the content-addressed model a tolerated (uncompressed-equal) divergence is a benign dedup-miss, not a fault (§4).

## 11. Phased roadmap (each phase behind a setting)

- **M1 — `PackedObjectDataPartStorage`, non-replicated, no Keeper.** Layout + manifest + footer + inline index/metadata + `LocalCatalog` (`refs/`, incl. persisted DROP tombstone). Solves #1(non-repl), #3, #4, #6, #7. Validate with the local-vs-CA oracle.
- **M2 — GC reusing the outdated lifecycle.** Reachability over `refs/` + grace via the lifecycle + `AbortIncompleteMultipartUpload` + dry-run + self-healing coordinator. Fault-inject the old-blob-newly-referenced and crash-before-commit windows.
- **M3 — replicated.** `KeeperCatalog`: keep header, append `manifest_hash`; commit under the existing `/log`; reads via catalog + shared blobs; enable single-replica merge (optional adopt-by-reference); **delete** the zero-copy subsystem. Solves #2, #5.
- **M4 — ops + DR.** DETACH/ATTACH(intra)/FETCH/FREEZE as ops (**FREEZE materializes bytes**); cross-table ATTACH = copy; persisted tombstones for DROP/REPLACE/MOVE; `/log`-replay DR rebuild with explicit DROP-PART folding.
- **M5 — multidisk + caching + stateless compute** (incl. the **ephemeral reader pin** and **lost-replica timeout**).
- **M6 — patch parts / lightweight delete** modeled as a separate reachability root class over the base set.
- **M7 — disaggregated merges + scale hardening.** Single-`/log` Keeper throughput (per-partition commit sharding); compliance fast-path; mixed-version migration (whole-table, layout-version flag on `/parts`, min-version gate — heterogeneous old+new replicas in one table **unsupported**).

## 12. Residual risks

1. **Cross-replica dedup precondition:** identical bytes require identical `<compression>`, `ratio_of_defaults_for_sparse_serialization`, `index_granularity(_bytes)`, marks-compression. With single-producer this is moot for replication (the loser references the winner); a *tolerated* divergence (uncompressed-equal, compressed-different) is a **benign, bounded dedup-miss** (two blob sets until the part is superseded), **not** a correctness bug. It bites only for cross-table/independent rebuild. Golden test must pin manifest determinism (incl. cross-version stability).
2. **Compact parts** get no carry-forward (one blob, full rewrite) — the dedup win is for Wide/large parts.
3. **DROP-PART mid-range race:** the persisted tombstone for a single mid-range part must reconcile with a concurrent merge that legitimately absorbs the data into a real covering part (the documented weak guarantee of `DROP_PART`). Must not strand, double-cover, or resurrect. Has a dedicated test; DR replay folds `DROP_PART` explicitly.
4. **Stateless / ref-less reader fence:** correctness depends on the ephemeral reader pin being created before any object is read and surviving in the MARK union; a missing pin → mid-scan deletion. Plus a crashed-but-not-dropped replica leaks its refs until a lost-replica timeout reaps them.
5. **Commit-time existence check** for carry-forward of many columns — batch it (one prefix LIST), skip for same-table (reachability already proves liveness), restrict HEAD to cross-table.
6. **FREEZE byte materialization cost:** FREEZE on this engine GET/copies real bytes into `shadow/` (it cannot be reference-only without breaking filesystem backups), so it is as expensive as a full copy of the frozen set. Acceptable, but it is no longer "free."
7. **Keeper throughput** — one `/log` + `/parts` per table; one-multi-per-commit; very high INSERT concurrency may need per-partition sharding (M7).
8. **GC coordinator stall** — self-healing + watchdog + full-`LIST` backstop; must not silently leak.
9. **Collision guard** — `(cityHash128, size)`; non-cryptographic threat model stated for adversarial INSERT.
10. **Transactions / metadata-only ALTER** — mutable fields in the ref, not the manifest; `writeMetadataVersion` redirected to a ref rewrite; in scope for M3.
11. **Patch parts / lightweight delete** — modeled as a separate reachability-root ref class referencing base-part blobs (§7); stays here until M6 proves the base-blob-pinning and patch-materialization GC paths.

### Bottom line

Content-addressed immutable blobs + the **existing** name/covering active-set (extended with **persisted covering tombstones** so removal is also "create a ref") + the **existing** outdated-parts lifecycle as the GC trigger + **ephemeral reader pins** for ref-less compute + single-producer as the recommended mode (divergence is a benign dedup-miss, not a fault) + reachability over the union of per-replica refs and reader pins. No journal, no per-blob locks, no refcounts. The storage change is **one new `IDataPartStorage`** that reinterprets hardlink-as-reference but overrides ~10 methods (including FREEZE, which must materialize bytes). Part types and the object store are unchanged. Parts are global, refs are per-replica, pool is per-catalog (cross-table = copy). Ships value at M1.

---

### Changelog from v2
- **Removal needs a primitive.** Split supersession into **additive** (INSERT/`MERGE_PARTS`/`MUTATE_PART` — genuinely "create one covering real ref"; `MergeTreePartInfo::contains` covers level↑/mutation↑) and **removal** (DROP PART / DROP_RANGE / REPLACE/MOVE source-side — **not** covering-expressible today because `getVirtualPartNames` returns `{}` for `DROP_PART` and `getActiveContainingPart` only walks Active real parts). Fix: **extend the existing empty tombstone part** (today created local/Outdated/uncommitted in `MergeTreeData.cpp` ~5692) into a **persisted, Active, covering tombstone ref** with no blobs. DROP PARTITION/range is clean; DROP PART mid-range must reconcile with a concurrent merge; REPLACE/MOVE is intrinsically multi-ref. Replicated `/log` entries kept verbatim; `LocalCatalog` DROP commits a tombstone; DR replay folds `DROP_PART` explicitly.
- **Reader fence for stateless compute.** `isSharedPtrUnique` is a per-process refcount, valid only for **ref-holding** replicas; a §7 ref-less reader is invisible to the GC union and can have objects deleted mid-SELECT. Added **ephemeral reader pins** (one-per-part or one-per-snapshot) included in the MARK union, auto-released on session loss. Corrected the v2 "grace ≠ longest SELECT" claim (true only for ref-holding replicas). Added **lost-replica-timeout** for crashed-but-not-dropped replicas' leaked refs.
- **Single-producer / divergence downgraded to an optional optimization** (was "must reintroduce `/merge_claims` + re-key divergence on `manifest_hash`"). Verified the commit-time check runs in **tolerant mode** (`checkEqual(..., true, ...)` comparing `uncompressed_hash_of_compressed_files`), so uncompressed-equal/compressed-different replicas pass → at worst a **benign, bounded dedup-miss** (P1, not P0; not "forever"). Homogeneous deterministic compression dedups with no check. Mitigation = enable the existing single-replica picker (`shouldMergeOnSingleReplica` reads only `execute_merges_on_single_replica_time_threshold`). Strict-mode and non-compressed-file checks still catch real divergence.
- **FREEZE must materialize real bytes.** `freeze`/`freezeRemote`/`clonePart` are independent `IDataPartStorage` virtuals, not built on `createHardLinkFrom`; `shadow/`'s contract is filesystem-readable hardlinks for backup pipelines. Moved FREEZE/clone from "unchanged" to "reworked at the `IDataPartStorage` seam."
- **Layering honesty.** Softened "ONE class, everything unchanged": the seam class overrides ~10 methods (`freeze`, `freezeRemote`, `clonePart`, `copyFileFrom`, `createProjection`, `replaceFile`, `moveFile`, `removeFile`, `iterate`), plus a part-layer `writeMetadataVersion` redirect (rewrite the ref). The hardlink→reference pivot is the heart, not the whole.
- **"metadata+index = one GET" softened** to "one GET for all part-open metadata+index." Manifest now inlines `primary.idx`/`partition.dat`/`minmax_*.idx`/`count.txt`/`ttl.txt`/`serialization.json` + a footer granularity summary (alongside the six service files); `.bin` and marks stay separate whole blobs. Noted `primary_key_lazy_load=true` default; this is what satisfies #7 **and** "FileCache unchanged" (no sub-object slicing).
- **New patch-parts / lightweight-delete section.** Patches live in a separate partition namespace (`ActiveDataPartSet` filters `isPatch`; `MergeTreePartInfo::isPatch`), applied on top of the base covering set, as a separate ref class whose manifest references base-part data. Modeled as a first-class reachability root (keeps its delta blobs and the referenced base blobs alive while active); added to residual risks until fully proven.
