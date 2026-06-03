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

A CA pool's identity is a **`pool_uuid`** minted at first pool claim and stored in `_pool_meta`. (Today
`_pool_meta` carries `owner_server_id` + `claimed_at_unix` but **no stable identity token** — the
adversarial review confirmed endpoint+prefix string-matching is unsafe: DNS aliases / path-vs-vhost S3 /
trailing-slash normalization give false negatives, and a shared proxy endpoint can give a false positive
→ relink to blobs that aren't there. So M-repl-1 adds a `pool_uuid` to `PoolMeta`, a `CURRENT_VERSION`
bump that fails closed on older readers.) Two replicas share a pool iff their `_pool_meta.pool_uuid`
match. Replicas advertise `pool_uuid` in the part-exchange capability handshake; endpoint+prefix is at
most a cheap pre-filter. A mismatch is **not** an error — it falls back to the byte fetch (§4), which is
correct on CA because the downloaded files are written as content-addressed blobs that
`condCreateIfAbsent`-dedup against the destination pool. Re-link is purely the same-pool optimization.

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
- **Receiver — PIN BEFORE PUBLISH (load-bearing, found by adversarial review).** The relink uploads no
  blobs, so unlike a normal write it creates **no `WriteSession` pin** — and the source replica may
  concurrently drop/merge-away its ref to `part_id`, after which a GC sweep (on any replica) would see
  `part_id` named by no ref and no session and reclaim its blobs in the window before the receiver
  publishes its ref → a dangling ref / lost part. So the receiver MUST, in order: (1) open a durable
  `WriteSession` whose `pending` = the source part's blob hashes **and** the `part_id`/manifest object
  (read `parts/<part_id>`), persisted to `sessions/<id>` BEFORE trusting the source; (2) re-validate the
  manifest + every blob it names is present in the pool (fail closed / fall back to byte fetch if not);
  (3) **publish the ref** `store/<self>/<uuid>/refs/<part>` → `part_id` + the per-ref sidecar (a fresh
  per-part mutable set — `uuid.txt`, `txn_version.txt`, `metadata_version.txt` — from the transferred
  header) via one `ContentAddressedTransaction`; (4) release the session. This mirrors the normal
  write's pin-before / release-after-ref discipline (the only reason INSERT/merge are cross-replica
  safe). No blob bytes are fetched. Commit with `renameTempPartAndAdd`/`renameTempPartAndReplace` as
  usual — on CA the rename is the `tmp_fetch`→active `moveDirectory` (the staging→active branch exists).
  (Relink also makes **B6 manifest determinism load-bearing**: the receiver publishes a ref to a
  `part_id` it did not compute, so the `(file,checksum)→part_id` mapping must be identical across
  servers — otherwise B names a `part_id` that doesn't resolve. Fail-closed, not corruption, but B6 must
  be closed before relink is trusted.)
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
- **Audit is PRIMARY work, not a confirmation (adversarial-review correction).** B21 was resolved for
  the *non-replicated* ALTER PARTITION stack (`checkAlterPartitionIsPossible`); the replication-**queue**
  entry points — `executeFetch`/`fetchPart`, `executeReplaceRange`/`REPLACE PARTITION`, queue-driven
  `MOVE PARTITION`, queue-driven merges/mutations, `cloneAndLoadDataPart` — are a DIFFERENT call stack
  that B33 was specifically guarding and that has **never executed on CA**. Treat each as
  guilty-until-audited: verify it routes through the working whole-part transaction or the relink fetch,
  **not** the per-file autocommit clone (the B21 corruption mode). Any path not yet proven safe must
  fail closed (a clear "not supported on CA yet (Bnn)" + a backlog item), never the silent-corruption
  path. (Good news from the review: the zero-copy `lockSharedData`/`unlockSharedData` calls that lifting
  B33 will reach are safe no-ops on CA — they early-return when `!supportZeroCopyReplication()`, which CA
  is — so no wrong ZK locks are created. The risk is purely the clone *data* paths above.)

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

**M-repl-1 must WIRE ON N-mounter mount (B32) — it is OFF by default (adversarial-review correction).**
`claimPoolOwnership` REJECTS a second concurrent mounter by default (`PoolMeta` throws
`SUPPORT_IS_DISABLED` "concurrent multi-mounter use … not supported" when a different `owner_server_id`
holds the marker and `!allow_shared`), so **the second replica fails to start** unless
`content_addressed_allow_shared_pool` is set. The mechanism to admit N mounters already exists (the
`allow_shared` path + the `condCreateIfAbsent`-based mounter registry), so the work is to enable it for
the replicated path (auto-enable when the table is `Replicated*MergeTree` on a CA disk, or require the
disk flag) — small, but it is a FIX, not a "confirm." With it on: (a) concurrent ref writes from
different replicas are safe (disjoint `store/<server>/…` prefixes); (b) concurrent shared-blob writes
are safe (`condCreateIfAbsent`/`If-None-Match` idempotent for identical content); (c) the fenced GC lock
already serializes one sweeper at a time across mounters; (d) the in-process B49/B52 pin protections are
per-server, so cross-replica blob safety rests entirely on the durable `WriteSession` pin — which is why
§4's relink-pin is mandatory.

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

1. **Pool identity + N-mounter mount.** Add `pool_uuid` to `PoolMeta` (version bump, fail-closed on old
   readers). Enable the `allow_shared_pool` path for `Replicated*MergeTree` on CA so the 2nd replica
   mounts. gtest: two mounters write disjoint refs + shared blobs + a coordinated (fenced) sweep; a
   `pool_uuid` round-trips and mismatched pools are detected.
2. **Fetch-by-relink with the mandatory pin.** Protocol capability advertising `pool_uuid`; sender
   same-pool detection; receiver = pin (`WriteSession` over the source blob hashes + part_id) →
   re-validate present → publish ref + sidecar → release. Byte-fetch fallback otherwise. gtest for the
   metadata-layer relink INCLUDING the pin-vs-concurrent-sweep race (a sweep between source-drop and
   ref-publish must not reclaim the pinned blobs).
3. **Lift B33 + audit the replication-queue clone paths** (`executeFetch`, `executeReplaceRange`,
   queue MOVE, queue merges/mutations, `cloneAndLoadDataPart`) — route through the whole-part
   transaction or fail closed. Confirm the zero-copy lock calls are CA no-ops.
4. Integration test (2-replica same-pool): relink moves no blobs (blob-object count unchanged),
   drop-on-one keeps it, drop-on-both reclaims; a merge on one replica fetched-by-relink by the other; a
   cross-pool pair exercising the byte-fetch fallback.
5. Stage E: un-gate ReplicatedMergeTree stateless tests on minio+CA; triage; extra rounds as needed.

## 11. Hardest decisions / risks {#risks}

Ranked by an adversarial review of this spec against the code:
1. **Relink pin (§4) — the one true data-loss hole.** Relink uploads no blobs ⇒ no `WriteSession` pin;
   a concurrent source-ref drop + GC sweep can reclaim the blobs before the receiver publishes its ref.
   The pin-before-publish discipline in §4 is MANDATORY and is the thing most likely to be gotten wrong.
2. **Pool identity = `pool_uuid` (§3), and N-mounter mount must be enabled (§6).** Endpoint+prefix is
   not a safe identity (false positives → fail-closed mis-relink); add `pool_uuid`. And the 2nd replica
   *throws* on mount until `allow_shared_pool` is wired on for the replicated path — a fix, not a check.
3. **Replication-queue clone-path audit (§5).** The queue entry points B33 guarded have never run on CA;
   guilty-until-audited, fail-closed, primary work.
- **Dead-replica stale refs.** A decommissioned replica's `store/<dead_server>/…/refs/` pin blobs
  forever (space leak only — **not** a correctness break; disjoint prefixes, never blocks writes). It is
  *unbounded* (grows per decommissioned replica × parts), so it is a tracked operability item, not
  silently open: deferred to a "drop a server's refs when its replica leaves ZK" pool-admin op (backlog).
