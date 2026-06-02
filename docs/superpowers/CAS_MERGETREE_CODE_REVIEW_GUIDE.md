# Content-Addressed MergeTree — Code Review / Navigation Guide

A reading order for the `cas-mergetree-poc` branch. The code is split into many small files; this is the **logical** order (follow the data flow), not alphabetical. All paths under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless noted.

## The 30-second mental model

A `content_addressed` disk is an `IMetadataStorage` over any `IObjectStorage` (local FS or S3). It stores, in one **global per-disk pool**:
- `blobs/<h0>/<h1>/<file_checksum>` — each part *file*'s bytes, keyed by content hash (deduplicated).
- `parts/<p0>/<p1>/<part_id>` — a **footer** per part: a `{logical_file → blob_key}` map. `part_id` = a deterministic rollup of the part's checksums.
- `store/<server_id>/<table_uuid>/refs/<part_name>` — a tiny **ref**: payload = the `part_id`. The set of refs is the active-part set.
- `store/<server_id>/<table_uuid>/files/<tail>` — table-level non-part files (`format_version.txt`), stored verbatim (not content-addressed).
- generic disk files (e.g. the startup access-check probe) — verbatim at `<key_prefix>/<path>`.

**Read** = `ref → part_id → footer → blob`. **Write** = upload blobs (hash-on-finalize) then publish `footer` + `ref` atomically at transaction commit. **Remove** = unlink the ref (synchronous); the now-unreferenced blobs/footers are reclaimed later by a background **reachability GC**.

---

## Reading order

### Part 0 — Why (skim first, ~10 min)
1. `docs/superpowers/specs/content_addressed_shared_mergetree_design.md` — the design + the layout + the GC model (grace-from-loss-of-reachability). The "why".
2. `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` — **read the known traps**: B18 (GC fail-close), B22 (key prefix/spill), B23 (mutable per-part state — a real hole), B25 (ServerUUID refs), B27 (server-readiness), B28 (ref-parse consistency). This tells you what is deliberately *not* solved.

### Part 1 — The vocabulary (everything builds on this)
3. `PoolPaths.h` / `PoolPaths.cpp` — the key scheme. `blobKey`/`partKey`/`refKey`/`refsPrefix`/`tableFileKey`/`diskFileKey` (all take `key_prefix` first; empty-prefix-safe via `withPrefix`) and the path parsers `parsePartFilePath`/`parseTableUuid`/`refsRootPrefix`/`partsPrefix`/`blobsPrefix`. **This is the schema** — understand it before anything else.
4. `Footer.h` / `Footer.cpp` — the per-part footer (the `file → BlobEntry` map) + its serialization. (Note B19: little-endian encoding is a pending item.)
5. `PartId.h` / `PartId.cpp` — `computePartId` (`PartId.cpp:22`): the rollup over sorted `(file, checksum)` **excluding** `uuid.txt`/`txn_version.txt`/`metadata_version.txt`. The exclusion set is correctness-relevant (it's what makes identical content dedupe and ties into B23).

### Part 2 — How a part is READ (resolution)
6. `ContentAddressedMetadataStorage.h` / `.cpp` — the `IMetadataStorage`. Read in this order:
   - `readRefPartId` (`.cpp:84`) → `partIdFromRefPayload` (shared with GC — B28).
   - `loadFooterOrThrow` (`.cpp:94`) — fail-close on a missing footer.
   - `getStorageObjects` (`.cpp:273`) — **the resolution heart**: classifies the path (part file / table file / generic) and returns the backing `StoredObject`(s).
   - `existsFile`/`getFileSize` (`.cpp:120`,`:166`) and `listDirectory`/`iterateDirectory` (`.cpp:217`,`:261`) — same classification, mirror.
   - `startup`/`shutdown` (`.cpp:55`,`:61`) — own/drive the GC thread (Part 4).

### Part 3 — How a part is WRITTEN
7. `ContentAddressedWriteBuffer.h` / `.cpp` — spill bytes to a **server-local scratch** dir while hashing (`HashingWriteBuffer`), then on `finalizeImpl` put-if-absent upload to `blobs/<hash>` and report the hash to the owning transaction. (B22(a): the scratch is a real local path, NOT the object key prefix.)
8. `ContentAddressedTransaction.h` / `.cpp` — accumulates `{file → BlobEntry}` and publishes at commit:
   - `writeFile` (`.cpp:53`) — three branches: part file → CA write buffer; table file → `tableFileKey` passthrough; else (generic) → verbatim `diskFileKey`.
   - `createHardLink` (`.cpp:98`) — mutation/ATTACH **carry-forward** (reuse the source's blob, no re-upload).
   - `commit` (`.cpp:220`) — **compute `part_id` → write `parts/<part_id>` footer → publish `refs/<part>` LAST** (footer before ref — ordering matters for GC).
   - `removeRecursive` (`.cpp:295`) — unlink refs/table/generic objects scoped by `table_uuid`; **never** deletes `blobs/` or `parts/` (deferred to GC).
   - `moveDirectory`/`moveFile` (`.cpp:135`,`:175`) — re-pin/re-key for the `tmp_insert_*`→final rename (fail-closed).
   - `setLastModified`/`chmod`/`setReadOnly`/`truncateFile` — no-ops (CA derives per-file metadata).
9. **The wiring** (small, in shared files):
   - `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:279` — the gated branch that routes a content-addressed write to the CA transaction's buffer.
   - `src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:79` — **`use_fake_transaction=false` for content_addressed** (the root-cause decision: CA needs real deferred transactions, like Keeper, so commit has a publish point). Worth understanding well.

### Part 4 — Garbage collection (SCRUTINIZE MOST — failure = silent data loss)
10. `Reachability.h` / `Reachability.cpp` — the pure algorithm: `markReachableBlobs(live_part_ids, resolve)` (`.cpp:6`) and `selectForSweep(unreferenced, first_unreachable, now, grace)` (`.cpp:18`, grace from first loss of reachability).
11. `PoolScan.h` / `PoolScan.cpp` — `listLivePartIds` (`.cpp:~59`, enumerate refs → `partIdFromRefPayload` at `.cpp:36`), `listKeysUnder`.
12. `ContentAddressedGC.h` / `ContentAddressedGC.cpp` — **`runSweepOnce`** (`.cpp:34`): the 5 steps (live set → reachable blobs → unreferenced = listed−reachable → `selectForSweep` → delete). The **fail-close ordering** (any throw in steps 1–2 aborts before any delete) is the key safety property.
13. `ContentAddressedGCThread.h` / `.cpp` — the background driver (mirrors `Replication/BlobKillerThread`): `BackgroundSchedulePool` task, fail-*safe* (a sweep exception logs and retries, deletes nothing), `startup`/`shutdown`/`triggerAndWait`.
14. `BlobRefIndex.h` / `.cpp` — **NOT on the live M1 path**. A Phase-1 seam (delta refcount) for the future B9 delta-driven sweep; the M1 sweep uses reachability instead. Has its own unit tests but is not wired into commit/remove. Skim and move on.

### Part 5 — Trivial integration points
- `src/Disks/DiskType.h:35` + `DiskType.cpp:22` — the `MetadataStorageType::ContentAddressed` enum + `"content_addressed"` string.
- `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:210` — `registerContentAddressedMetadataStorage` (computes the scratch path + GC settings + passes `ServerUUID`/context).
- `src/Storages/MergeTree/MergeTreeData.cpp:6519` + `DiskObjectStorage.cpp:711` — `case MetadataStorageType::ContentAddressed:` arms in two switches (mechanical).

### Part 6 — Tests (the oracle)
- `src/Disks/tests/gtest_content_addressed.cpp` — Phase-1 pure logic (footer, reachability, refcount).
- `src/Disks/tests/gtest_content_addressed_metadata.cpp` — the big one: read/resolve, write+commit, dedup, carry-forward, removeRecursive-keeps-blobs, and the GC sweep cases (grace, dedup-kept, reachable-again, fail-close-on-missing-footer).
- `tests/queries/0_stateless/04278_content_addressed_disk.sql` — full lifecycle vs a normal table (oracle).
- `tests/queries/0_stateless/04279_content_addressed_gc.sql` — correctness under active GC.
- `tests/integration/test_content_addressed_s3/` — CA over MinIO.
- `tests/integration/test_content_addressed_gc_s3/` — polls the bucket; proves dropped blobs are actually reclaimed.

---

## The handful of spots that matter most (review these closely)

| Concern | Where | What to check |
|---|---|---|
| GC must never delete a live blob | `ContentAddressedGC.cpp:34` `runSweepOnce` | Fail-close ordering (throw before delete); `unreferenced` built ONLY from `parts/`+`blobs/` listings; grace applied. |
| Commit atomicity | `ContentAddressedTransaction.cpp:220` `commit` | Footer written **before** ref; `part_id` computed over the right file set. |
| Removal scope | `ContentAddressedTransaction.cpp:295` `removeRecursive` | Scoped by `table_uuid`; never touches `blobs/`/`parts/`; doesn't delete another table's refs. |
| GC vs read agreement | `PoolScan.cpp:36` `partIdFromRefPayload` (shared) | GC's live set and the read path resolve a ref to the **same** part_id (B28). |
| part_id determinism | `PartId.cpp:22` `computePartId` | The exclusion set; relates to B23 (mutable per-part state currently in the footer, not the ref). |
| Why real transactions | `RegisterDiskObjectStorage.cpp:79` | The `use_fake_transaction=false` decision and its rationale. |

## Honest caveats (so review focus is calibrated)
- Tests are single-node, single-writer, short-lived, happy-path. They do **not** exercise: concurrent reader-vs-GC, crash between footer-write and ref-publish under load, the grace>commit-window invariant under a slow insert, or any concurrency. These are the likeliest places for a latent bug.
- B23 (mutable per-part state) is an unsolved correctness gap, not deferred polish.
- Everything (design, code, tests) was AI-authored end to end; the tests may share blind spots with the implementation.
