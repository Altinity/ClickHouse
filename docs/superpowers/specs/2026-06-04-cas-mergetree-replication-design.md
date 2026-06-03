---
description: Design spec for ReplicatedMergeTree on a content-addressed shared-pool disk — fetch-by-relink instead of byte download (the CA analogue of zero-copy replication).
sidebar_label: 'CAS MergeTree replication'
sidebar_position: 5
slug: /superpowers/specs/cas-mergetree-replication
title: 'Content-Addressed MergeTree — ReplicatedMergeTree Design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — ReplicatedMergeTree Design {#cas-mergetree-replication}

**Status:** design spec, awaiting review (authored autonomously; the brainstorm sounding-board was a
read-only code investigation, not the user). **Date:** 2026-06-04. **Backlog:** B1 (replication), B33
(the current `ReplicatedMergeTree`-on-CA gate), B32 (multi-mounter pool coordination), B11 (Keeper
accelerator).

## 1. Goal and scope {#goal}

Run `ReplicatedMergeTree` on a content-addressed (CA) shared-pool disk. A replica that needs a part it
lacks should **publish a ref to the already-present shared blobs (re-link)** instead of downloading the
bytes — the CA analogue of zero-copy replication's metadata-only fetch. Other replication operations
work as normal, but the data-moving file operations become no-ops because the content is already in the
shared pool.

**M-repl-1 scope (this spec):** lift the B33 gate; fetch-by-relink for the same-pool case with a
byte-fetch fallback; confirm/extend the GC to be safe with N replicas mounting one pool; route the
replication-queue clone paths through the working whole-part CA transaction; a 2-replica same-pool
integration test. **North-star follow-on:** the ReplicatedMergeTree stateless tests pass on the
minio+CA disk (Stage E of the overnight plan).

**Deferred (backlog, each with a plug-in point):** dead/decommissioned-replica ref cleanup (stale refs
pin blobs); a Keeper-accelerated ref index (B11); cross-pool fetch *optimization* (cross-pool already
works correctly via the byte-fetch fallback + content-addressed dedup — it is just not relink-optimized);
multi-region / multiple pools per table.

## 2. The key insight — why CA replication is simpler than zero-copy {#key-insight}

Zero-copy replication shares raw S3 objects across replicas and needs a **ZooKeeper lock per
part-per-disk** (`zero_copy_<disk>/<uuid>/<part>/<part_id>/<replica>`) to decide when a shared object is
safe to delete ("is any replica still holding it?"). It is intricate because the object store has no
intrinsic notion of who references what.

Content-addressing already provides that notion. On a CA disk:
- All replicas on the same pool see the **same content-addressed blobs** (`blobs/<hash>`), inherently
  deduplicated. A blob written by replica A is immediately the blob replica B would write for identical
  content (same hash; `condCreateIfAbsent` makes the second write a no-op).
- A part is referenced by a **ref object** in the pool: `store/<server_id>/<table_uuid>/refs/<part>` →
  `part_id`. Refs are **per-server** (each replica has its own `store/<server_id>/…/refs/` namespace).
- **The GC already treats the UNION of all servers' refs as reachability roots.** Confirmed:
  `ContentAddressedGC` scans `refsRootPrefix == store/` (the root over *every* server), so
  `markReachableBlobs` is rooted at every replica's refs. A blob is reachable iff **any** server's ref
  names it.

Therefore the refs in the shared pool **are** the cross-replica reference tracking — no ZooKeeper
blob-lock is needed. A "fetch" becomes: replica B publishes its own ref
(`store/<B>/<uuid>/refs/<part>` → the source part's `part_id`) pointing at blobs that are already in the
pool. When B later drops the part it unlinks only its own ref; A's ref keeps the blobs alive; the blobs
are reclaimed only when **no** replica references the `part_id` (the existing union-of-refs +
fenced-GC-lock + session-pin machinery, M8). This is both simpler and safer than the ZK-lock scheme.

## 3. Pool identity {#pool-identity}

A CA pool's identity is its **(object-storage endpoint + key prefix)** — the same `_pool_meta` object
that M8 uses for single-owner self-check. Two replicas share a pool iff they resolve to the same
endpoint+prefix. The fetch path must verify same-pool before re-linking (§4). Replicas advertise their
pool identity in the part-exchange capability handshake (and/or in their ZooKeeper replica metadata).
A mismatch is **not** an error — it falls back to the byte fetch (§4), which is correct on CA because
the downloaded files are written as content-addressed blobs that `condCreateIfAbsent`-dedup against the
destination pool. Re-link is purely the same-pool optimization.

## 4. Fetch-by-relink protocol {#fetch}

Mirror the existing zero-copy fetch shape in `DataPartsExchange` (`Service`/`Fetcher`), which already
transfers metadata instead of data under protocol version 6
(`REPLICATION_PROTOCOL_VERSION_WITH_PARTS_ZERO_COPY`) and negotiates a `remote_fs_metadata` capability.

- **Capability handshake.** The receiver advertises a CA capability carrying its pool identity, e.g.
  `content_addressed:<endpoint>|<key_prefix>` (alongside the existing `remote_fs_metadata` disk-type
  list). A new protocol version constant (e.g. `…WITH_CA_RELINK`) gates it.
- **Sender.** If the requested part is on a CA disk and the receiver's advertised pool identity equals
  the sender's pool identity, the sender sends a lightweight payload: the part's `part_id` plus the
  part header it already sends for a normal fetch (checksums, columns, part type, UUID if protocol ≥ 5,
  `metadata_version`, TTL infos, projections list) — but **no file data**. Otherwise it sends the part
  the normal way (full byte stream), exactly as today.
- **Receiver.** On the relink payload: construct the part in a `tmp_fetch_<part>` directory whose
  `IDataPartStorage` is the local CA disk, then **publish a ref**: write
  `store/<self>/<uuid>/refs/<part>` → `part_id` and the per-ref sidecar (a fresh per-part mutable set —
  `uuid.txt`, `txn_version.txt`, `metadata_version.txt` — from the transferred header), via one
  `ContentAddressedTransaction` (the same whole-part commit INSERT/merge use). No blob bytes are
  fetched; the blobs are already in the pool keyed by the hashes inside `parts/<part_id>`. Commit with
  `renameTempPartAndAdd`/`renameTempPartAndReplace` as usual — on CA the rename is the
  `tmp_fetch`→active `moveDirectory` (the staging→active branch already exists).
- **No-op file operations.** The fetch's per-file download/write calls become no-ops on the relink path:
  the manifest already names the blobs and `getStorageObjects` resolves them. Only the ref publish +
  sidecar write actually do work. (This is the "some file operations become no-op" the goal calls for.)
- **Fallback.** If the receiver lacks the capability, or the pools differ, or the relink payload can't
  be satisfied (e.g. the sender's part isn't content-addressed), the normal byte fetch runs. On CA the
  written files content-address and dedup; correctness is unaffected.

## 5. Lift the B33 gate {#lift-gate}

`StorageReplicatedMergeTree` currently throws `SUPPORT_IS_DISABLED` for any `isContentAddressed()` disk
at CREATE/ATTACH (the B33 gate). The original B33 reason was that the replication queue's clone paths
(fetch fallback, queue-driven `REPLACE`/`MOVE`/`ATTACH PARTITION`) reached the per-file-autocommit
clone (B21) and corrupted CA parts. That hazard is now closed: the partition-clone ALTERs route through
the whole-part CA transaction (M9 W2), and the fetch is relink (§4). So:

- Remove the blanket B33 throw. Allow `ReplicatedMergeTree` on a CA disk.
- Audit every replication-queue data path that produces or clones a part —
  `executeFetch`/`fetchPart`, `executeReplaceRange`/`REPLACE PARTITION`, `MOVE PARTITION`, queue-driven
  merges/mutations, `cloneAndLoadDataPart` — and confirm each routes through the working whole-part
  transaction or the relink fetch, **not** the per-file autocommit clone. Any path that cannot yet be
  satisfied must fail closed (a clear "not supported on CA yet (Bnn)" with a backlog item), never the
  silent-corruption path.

## 6. Cross-replica GC safety {#gc}

Already provided by the union-of-refs reachability (§2) plus the M8 machinery:
- **Roots = every server's refs** (`store/*/refs/`), so a part fetched-by-relink onto replica B is a
  root via B's ref, and a part written by A is a root via A's ref. Dropping one replica's ref never
  reclaims a blob another replica still references.
- **Fenced GC lock** (M8 `pool/gc.lock` + `pool/fence/<n>`): at most one sweeper at a time across the
  pool; the sweeper re-validates roots (all refs ∪ live sessions) under the lock before each delete,
  with the fence guard. This already serializes GC across replicas sharing the pool.
- **Session pins** (M8 `sessions/<id>`): a replica writing/merging a part pins its in-flight blobs so a
  sweep on another replica treats them as reachable before the ref is published.

**The one thing M-repl-1 must confirm/harden (B32):** M8's pool self-check (`_pool_meta` +
`_pool_meta.mounters/<server_id>`) was validated **single-server-shared**. With N replicas mounting one
pool concurrently, confirm: (a) `_pool_meta` admits N mounters under the `allow_shared_pool` flag rather
than rejecting the second as a split-brain; (b) concurrent ref writes from different replicas are safe
(they are — disjoint `store/<server>/…` prefixes); (c) concurrent shared-blob writes are safe (they are
— `condCreateIfAbsent` / `If-None-Match` is idempotent for identical content); (d) the fenced GC lock
and session-pin TTLs behave with N live mounters. If any of these is not yet true, hardening it is part
of M-repl-1.

## 7. Per-part mutable state {#mutable-state}

A fetched part's `uuid.txt`/`txn_version.txt`/`metadata_version.txt` live in the per-ref sidecar (not
content-addressed, per-server). The relink receiver writes them from the transferred part header
(the UUID from the protocol ≥ 5 field; `metadata_version` from the header; `txn_version` =
non-transactional), exactly as the normal fetch sets them today — just into the CA sidecar instead of
per-file objects.

## 8. Error handling {#errors}

Fail-closed throughout: a missing ref/blob throws `FILE_DOESNT_EXIST`/`CORRUPTED_DATA` (never a silent
empty part). A relink that cannot verify the source `part_id` is present in `parts/` falls back to the
byte fetch. A queue clone path that isn't yet CA-safe fails closed with a backlog-referenced message,
never the per-file-autocommit corruption path.

## 9. Testing {#testing}

- **gtest:** the relink ref-publish at the metadata layer — seed a pool with a committed part_id +
  blobs under server A's refs, publish a ref under server B for the same part_id, assert B resolves and
  reads the part with no new blobs written; assert the GC keeps the blobs while either ref exists and
  reclaims them only when both are unlinked.
- **Integration (2-replica same-pool cluster):** INSERT on replica A; replica B fetches the part —
  assert (a) B reads it back identical, (b) **no new blob objects** were created in the pool by the
  fetch (object count under `blobs/` unchanged), proving relink not download; DROP the part on A —
  assert B still reads it; DROP on B — assert the blobs are reclaimed (no leftovers). A merge on one
  replica then fetched by the other. A cross-pool pair to exercise the byte-fetch fallback (correct,
  just creates blobs that dedup).
- **Stage E:** un-gate the `ReplicatedMergeTree` stateless tests and run on the minio+CA job; triage.

## 10. Plan phasing {#phasing}

1. Confirm/harden N-replica pool coordination (B32) — `_pool_meta` admits N mounters; gtest for two
   mounters writing disjoint refs + shared blobs + a coordinated sweep.
2. Fetch-by-relink: protocol capability + sender same-pool detection + receiver ref-publish; byte-fetch
   fallback; gtest for the metadata-layer relink.
3. Lift B33 + audit/route the replication-queue clone paths through the whole-part transaction (fail
   closed on any not-yet-safe path).
4. Integration test (2-replica same-pool): relink moves no blobs, drop-on-one keeps it, drop-on-both
   reclaims.
5. Stage E: un-gate ReplicatedMergeTree stateless tests on minio+CA; triage; extra rounds as needed.

## 11. Hardest decisions / risks {#risks}

- **N-replica pool coordination (B32).** The biggest unknown; §6 lists the four properties to confirm.
  If M8's self-check rejects a second concurrent mounter, M-repl-1 must relax it to the shared-pool case.
- **Dead-replica stale refs.** A decommissioned replica's `store/<dead_server>/…/refs/` pin blobs
  forever. Deferred — needs a "drop a server's refs when its replica leaves ZK" cleanup (a pool-admin
  op). Backlog item; until then a removed replica leaks its referenced blobs (safe, just not reclaimed).
- **Same-pool detection correctness.** Mis-detecting two *different* pools as the same would relink to
  blobs that don't exist on the target → fail-closed read errors (not corruption). The pool-identity
  comparison (endpoint+prefix, cross-checked against `_pool_meta`) must be exact; when unsure, fall back
  to byte fetch.
