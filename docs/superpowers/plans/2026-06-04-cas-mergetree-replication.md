# Content-Addressed MergeTree — ReplicatedMergeTree Implementation Plan

> **For agentic workers:** integration-heavy plan. Each phase is reproduction-driven: implement against
> the live interfaces (the spec lists the exact seams), build, verify empirically. Use the
> `cas-test-triage` discipline for any stateless/integration run (foreground, bounded `timeout`,
> non-empty selector, never `clickhouse local`, no background hangs). Build to a log, no `-j`/`nproc`.

**Goal:** `ReplicatedMergeTree` on a content-addressed shared-pool disk, where a fetch re-links to
already-present shared blobs instead of downloading bytes (CA analogue of zero-copy), safe across N
replicas sharing one pool.

**Spec:** `docs/superpowers/specs/2026-06-04-cas-mergetree-replication-design.md` (read it — esp. §4's
mandatory relink-pin, §3 pool_uuid, §6 N-mounter enable, §5 queue-clone audit).

**Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Key seams (from the code map):**
- Fetch protocol: `src/Storages/MergeTree/DataPartsExchange.cpp` — `Service::processQuery`/`sendPartFromDisk` (sender), `Fetcher::fetchSelectedPart`/`downloadPartToDisk`/`downloadBaseOrProjectionPartToDisk` (receiver); protocol version consts at lines ~70-78 (zero-copy = v6).
- B33 gate: `src/Storages/MergeTree/StorageReplicatedMergeTree.cpp:~450-465` (throw on `disk->isContentAddressed()`).
- Pool meta + N-mounter self-check: `…/ContentAddressed/PoolMeta.{h,cpp}` (`claimPoolOwnership`, the `allow_shared` path, `CURRENT_VERSION`).
- CA transaction + session pin: `…/ContentAddressed/ContentAddressedTransaction.cpp` (`recordBlobInSession`/`persistSession`/`releaseSession`, the commit ref-publish), `WriteSession.{h,cpp}`.
- GC reachability (union of `store/*/refs/`): `…/ContentAddressed/ContentAddressedGC.cpp`.
- Pool paths: `…/ContentAddressed/PoolPaths.{h,cpp}`.
- zero-copy lock calls (must be CA no-ops): `StorageReplicatedMergeTree.cpp` `lockSharedData`/`unlockSharedData` (early-return on `!supportZeroCopyReplication`).

---

## Phase 1 — Pool identity (`pool_uuid`) + N-mounter mount

**Why first:** without it the 2nd replica throws on mount; without `pool_uuid` relink can't safely detect same-pool.

### Task 1.1 — add `pool_uuid` to `PoolMeta`
**Files:** `…/ContentAddressed/PoolMeta.{h,cpp}`, gtest `…/tests/gtest_content_addressed_metadata.cpp`.
- Add a `pool_uuid` field (a 128-bit id rendered as a string) to the `PoolMeta` struct + its serialize/deserialize; bump `CURRENT_VERSION` (fail-closed on older readers — mirror the existing version-gate). Mint the `pool_uuid` at first claim (`condCreateIfAbsent(_pool_meta)` success path); on re-mount/accept, READ the existing `pool_uuid` (do not re-mint). The id source: since `Math.random`/time are restricted in some contexts but this is server code, use the existing id-generation the codebase uses for such markers (e.g. a UUID from `ServerUUID`/`generateRandomUUID` equivalent already used elsewhere in CA) — find what M8 uses and mirror.
- gtest: claim a pool → `pool_uuid` non-empty + stable across re-open; an old-version `_pool_meta` blob fails closed; two different pools (different prefixes) get different uuids.
- Build `unit_tests_dbms`, run `--gtest_filter='ContentAddressed*'`, commit.

### Task 1.2 — enable N-mounter mount for replicated CA tables
**Files:** `PoolMeta.cpp` (`claimPoolOwnership`), the disk/setting plumbing for `content_addressed_allow_shared_pool`, `StorageReplicatedMergeTree.cpp` (where the CA disk is mounted/validated).
- Today `claimPoolOwnership` throws on a second mounter unless `allow_shared`. Wire `allow_shared` on for the `Replicated*MergeTree`-on-CA path: simplest correct option — when a table is Replicated and on a CA disk, pass `allow_shared_pool=true` to the metadata storage (so concurrent replicas mount the same pool). Confirm the mounter registry (`_pool_meta.mounters/<server_id>`) records each replica.
- Verify: a gtest or a local 2-"server" simulation (two `ContentAddressedMetadataStorage` instances with different `server_id` on the same pool prefix, both with `allow_shared_pool`) both mount, write disjoint refs, write the same blob (idempotent `condCreateIfAbsent`), and a fenced sweep from one sees both servers' refs as roots.
- Commit.

---

## Phase 2 — Fetch-by-relink (with the mandatory pin)

### Task 2.1 — protocol capability + pool_uuid handshake
**Files:** `DataPartsExchange.cpp` (`Fetcher::fetchSelectedPart` request build; `Service::processQuery` capability parse), a new protocol version constant.
- Receiver advertises a CA capability carrying its `pool_uuid` (e.g. append `content_addressed:<pool_uuid>` to the `remote_fs_metadata`-style capability list, or a new query param). Add `REPLICATION_PROTOCOL_VERSION_WITH_CA_RELINK` (next int) and negotiate it.
- Sender: detect (part is on a CA disk) AND (receiver pool_uuid == sender pool_uuid). If so, take the relink send path (Task 2.2); else the normal byte send (unchanged).
- Verify the handshake with a unit-level test if feasible; else covered by the integration test (Task 2.4).

### Task 2.2 — relink send + receive (PIN BEFORE PUBLISH — load-bearing, spec §4)
**Files:** `DataPartsExchange.cpp` sender (`sendPartFromDisk` relink branch) + receiver (`downloadPartToDisk` relink branch); `ContentAddressedTransaction` / `WriteSession` for the pin + ref publish.
- **Sender (relink):** send the part's `part_id` + the part header it already sends (checksums, columns, part type, UUID, `metadata_version`, TTL infos, projections list) — NO file data.
- **Receiver (relink), in strict order:**
  1. Open a durable `WriteSession` whose `pending` = the source part's blob hashes (from the transferred manifest/part header) AND the `part_id`/manifest object; persist `sessions/<id>` BEFORE trusting the source. (Reuse the `WriteSession` write path; you may need a "pin an existing part_id's blobs" entry distinct from the write-buffer's per-blob pin.)
  2. Re-validate the manifest (`parts/<part_id>`) and every blob it names is present in the pool. If anything is missing → release the session and FALL BACK to byte fetch (do not publish a dangling ref).
  3. Publish the ref `store/<self>/<uuid>/refs/<part>` → `part_id` + the per-ref sidecar (`uuid.txt`/`txn_version.txt`/`metadata_version.txt` from the transferred header) via one `ContentAddressedTransaction`.
  4. Release the session.
  Construct the in-memory part (build from the header), commit via `renameTempPartAndAdd`/`renameTempPartAndReplace` (the `tmp_fetch`→active CA `moveDirectory` staging→active branch).
- **No blob bytes move on this path.** The per-file download writes are skipped.
- **Fallback** (no capability / pool mismatch / missing blob): the existing byte fetch (already transaction-wrapped on CA, so files content-address + dedup — verified safe).
- gtest (metadata layer): seed a pool with server A's committed part_id+blobs+ref; simulate the relink — open the pin, publish server B's ref to the same part_id; assert B resolves+reads the part with ZERO new blobs; assert the pin protects the blobs across a sweep triggered with A's ref dropped BEFORE B's ref is published (the race the pin closes); assert dropping both refs reclaims the blobs.
- Build server + unit, verify, commit.

### Task 2.3 — B6 manifest determinism check (prereq for trusting relink)
- Confirm the `(file,checksum)→part_id` mapping is deterministic across servers (so B's ref to a sender-computed `part_id` resolves). If B6 is still open/uncertain, add a gtest pinning determinism (two independent manifest builds of the same file set → same part_id) and note status. If non-deterministic, relink must transfer the manifest bytes (not just part_id) and the receiver writes `parts/<part_id>` too — adjust Task 2.2 accordingly. Document the finding.

### Task 2.4 — 2-replica same-pool integration test
**Files:** `tests/integration/test_cas_replicated_relink/` (mirror an existing 2-replica integration test).
- A cluster of 2 replicas pointed at the SAME minio bucket+prefix CA pool. INSERT on replica1; replica2 fetches. Assert: (a) replica2 reads identical data; (b) the pool's `blobs/` object count is UNCHANGED by the fetch (relink, not download) — query via minio/`system.remote_data_paths` or count objects; (c) DROP the part on replica1 → replica2 still reads it; (d) DROP on replica2 → blobs reclaimed (no leftovers after GC grace). Plus: a merge on replica1 fetched-by-relink on replica2.
- Run via `python -m ci.praktika run "integration" --test test_cas_replicated_relink` (bounded). Commit.

---

## Phase 3 — Lift B33 + audit the replication-queue clone paths

### Task 3.1 — lift the gate
**Files:** `StorageReplicatedMergeTree.cpp:~450-465`.
- Remove the blanket throw on `isContentAddressed()`. Allow Replicated CA tables to be created (with `allow_shared_pool` wired from Phase 1.2). Keep failing closed on any specific not-yet-safe op (Task 3.2).

### Task 3.2 — audit every queue clone/data path (guilty-until-audited, fail-closed)
For EACH of: `executeFetch`/`fetchPart`, `executeReplaceRange`/`REPLACE PARTITION`, queue-driven `MOVE PARTITION`, queue-driven merges, queue-driven mutations, `cloneAndLoadDataPart` — trace on a CA disk and confirm it routes through the working whole-part transaction (or the relink fetch), NOT the per-file autocommit clone (B21 mode). For any path not yet safe: fail closed with `SUPPORT_IS_DISABLED` "not supported on CA yet (Bnn)" + a backlog item (do NOT leave a silent-corruption path). Confirm `lockSharedData`/`unlockSharedData` are CA no-ops (early-return on `!supportZeroCopyReplication`). Add a stateless/integration probe per path where practical.
- Build, verify, commit (one commit per path-cluster is fine).

---

## Phase 4 — Cross-replica GC safety verification

GC union-of-refs is already in place (`refsRootPrefix == store/`). This phase VERIFIES it end-to-end for the replicated scenario and confirms the fenced lock + session pins behave with N mounters.
- Integration/gtest: two mounters, one runs a sweep while the other holds a fresh relink pin → the pinned blobs survive; a part referenced only by replica A's ref is NOT reclaimed by replica B's sweep; after both refs gone + grace, reclaimed. (Much of this is covered by Task 2.2's gtest + 2.4's integration test — this phase is the explicit cross-replica GC assertion, add what's missing.)
- Document the dead-replica stale-ref leak as a backlog item (already in the spec §11) with the cleanup-op plug-in point.

---

## Phase 5 — Un-gate ReplicatedMergeTree stateless tests on minio+CA (Stage E)

- Enumerate stateless tests tagged `no-content-addressed-storage` that use `ReplicatedMergeTree`/`Replicated*` (and `ON CLUSTER` replicated patterns). Un-tag.
- Run under the minio+CA job `Stateless tests (arm_binary, content_addressed s3 storage, parallel)` (Stage B's config) in bounded batches. Triage: real CA-replication bugs (fix or precise re-gate + backlog), orthogonal (Keeper/cluster infra not about CA), flaky, disk-specific. Iterate until green-except-documented.
- Update backlog: B1/B33 → DONE for M-repl-1 scope; list re-gated tests + reasons; new deferrals (dead-replica cleanup, Keeper accel, cross-pool relink) with plug-in points. Commit + push.

---

## Done criteria
- 2 replicas on one CA pool: a fetch re-links (moves no blob bytes), the fetched part reads back, and cross-replica drop/GC reclaims only when no replica references the part.
- B33 lifted; every queue clone path routes through the whole-part transaction or fails closed.
- ReplicatedMergeTree stateless tests pass on minio+CA except documented orthogonal/disk-specific ones.
- No new pool leftovers; the relink pin closes the concurrent-drop-vs-sweep race (the one true data-loss hole).
