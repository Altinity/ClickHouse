# CA build: precommit-first — no pool operation before the precommit (B188)

**Status:** design for review
**Date:** 2026-06-21
**Backlog:** B188. Model: `docs/superpowers/models/CaBuildRootPrecommit.tla` (B171). Relates to B172.

## Requirement (operator, non-negotiable)

The build must **precommit its full manifest tree before it touches the pool**. Every pool
operation that can observe-or-mutate a content object — conditional `PUT`, dedup `HEAD`, the 412
retry loop, `resurrect`, adopt-observe — MUST run **only after the precommit edge is durable**, so
the entire closure is protected by reachability from a durable build root the moment any object is
created, observed, or relied upon. This is the model's `INV_BUILDROOT_PROTECTS` taken literally:
protect first, then act.

## Why this is possible

`CaContentWriteBuffer::finalizeImpl` **spills each content file to a local temp file and computes
its CityHash128 before any upload** (`ContentAddressedWriteBuffers.cpp:55-68`). So at the end of
staging every file's content hash is known from local temp files — the full manifest tree is
computable with **zero** pool operations. There is no chicken-and-egg: hashes come from the local
spill, not from uploading.

## Current behavior (the bug)

Staging issues pool operations **before** any precommit:
- `writeFile` content path: the spill-finalize callback calls `Build::putBlob` **immediately**
  (upload + conditional-PUT; on 412 → `observeAndAdmit` HEAD → adopt/resurrect), then **deletes the
  temp file** (`ContentAddressedWriteBuffers.cpp:68`).
- adopt path (`createHardLink`/`moveFile`/`clonePart` → `reuseBlob`): an eager `observeAndAdmit`
  HEAD, fatal `FILE_DOESNT_EXIST` on absent.

`precommit(tree)` only runs at commit, in `publishStaging` (`ContentAddressedTransaction.cpp:188`),
**after** all of the above. So a hot duplicate-content blob GC'd during the unprotected staging
window makes an adopt fail fatally (the 24h-soak failure, B188), and even written-blob dedup
HEAD/resurrect run unprotected.

## Design — defer ALL pool I/O to after the precommit

### Staging phase: local-only, no pool operations
1. **Content `writeFile`** (`CaContentWriteBuffer` callback): instead of `putBlob`, **record a
   pending blob** `{hash, temp_path, size}` in the part staging and add the `Blob` tree entry.
   **Do not upload. Do not delete the temp file** — ownership of the temp file transfers to the
   transaction (cleaned up at commit/abort/destruction).
2. **Adopt** (`createHardLink`/`moveFile`/`clonePart` content-blob branches): call
   `Build::adoptEvidence(entry)` — record the tokenless W-EVIDENCE dep by hash, **no HEAD**. Delete
   the `body_recreatable`/`reuseBlob` calls at these sites.
3. Mutable per-part files and inline entries: unchanged (they are payload/overlay, not pool blobs).

After staging: the part has a complete set of tree entries (all hashes known), a set of pending
local temp files for the to-be-uploaded blobs, and tokenless deps for the adopted blobs. **No pool
object has been created, HEADed, or resurrected.**

### Commit phase (`publishStaging`): protect, then materialize
1. `putTree(entries)` — assemble the manifest tree. (W-TREE-BUILD is satisfied: written blobs are
   added as deps when their pending entry is recorded; adopted blobs via `adoptEvidence`. See
   "Dep bookkeeping" below.)
2. **`precommit(tree)`** — publish the build-root → tree edge. The full closure (written + adopted)
   is now GC-protected by reachability. **This is the upfront precommit.**
3. **Materialize each pending written blob:** `putBlob(hash, source-from-temp_path)` — the upload,
   the conditional-PUT, the 412 dedup branch (HEAD + adopt/resurrect) **all run here, with the
   precommit already durable**. The W-FRESH-TAG retry loop is unchanged; it just executes under
   protection.
4. **`publish`** — the fail-closed gate (`gateCheckDeps`/`revalidateDeps`) re-proves the closure
   present, resurrects condemned members, and on a genuinely-absent member throws **retryable
   `ABORTED`** (`CasBuild.cpp:839`) — never a fatal dangle. (`INV_COMMIT_FAILCLOSED`.)
5. Clean up the part's temp files; on success drop the precommit edge.

### Dep bookkeeping (the one subtlety)
`putTree` requires every child to already be in the build's dep set (W-TREE-BUILD). Today `putBlob`
records the (tokened) dep as a side effect of uploading. Since we now upload *after* `putTree`, the
written-blob dep must be recorded at **staging** time (when the pending entry is added) so `putTree`
sees it, then **finalized** (token stamped) when `putBlob` runs post-precommit.

Approach: at staging, record the written blob as a **tokenless** dep (same shape as `adoptEvidence`
— `DepEntry{Blob, std::nullopt, view_round, size}`). At commit, `putTree` → `precommit` → then for
each pending blob call `putBlob`, which **overwrites** the dep with the real tokened entry (it
already does `deps[{Blob,hash}] = DepEntry{..., tok, ...}` on a fresh upload, and `observeAndAdmit`
on the 412 branch). So the dep starts tokenless (pre-upload, for `putTree`) and becomes tokened
(post-upload). The publish gate runs last and sees the final tokened/tokenless mix, exactly as today.

### Eager-publish-at-rename (B151) interaction
`moveDirectory` can publish a part at the lock-free rename (B151), bypassing `commit()`'s
`publishStaging`. That path must follow the same order: assemble tree → precommit → materialize
pending blobs → publish. The pending-blob materialization step is invoked from the shared publish
helper so both the eager-rename path and the `commit()` path go through it. (Implementation: move
the "materialize pending blobs" loop into the same place that does `putTree`/`precommit`/`publish`,
so there is a single ordered publish routine.)

## What does NOT change
- `putBlob`'s internals (W-FRESH-TAG, the bounded 412 retry loop, dedup-as-cold-reuse, P1/P2
  HEAD-before-PUT) — unchanged; they simply run after precommit.
- The publish gate — unchanged; it already does present⇒admit / condemned⇒resurrect / absent⇒
  retryable `ABORTED`.
- Written-blob protection model — was heartbeat (tokened dep); now *also* covered by the precommit
  reachability, strictly stronger.
- The pool layout, codecs, GC.

## Tradeoff
Temp files for a part's content blobs are held in local scratch until commit instead of being
streamed-and-freed per file. Peak scratch ≈ the part size, which the merge/insert already
materializes locally — acceptable. If local-disk pressure becomes an issue, the **B172** variant
parks bytes in a private S3 staging prefix (still not a *pool* object) and server-side-copies them
into the pool after precommit; same precommit-first guarantee, no extra local disk. Out of scope here.

## Testing
- **Reproduce B188 deterministically:** a build adopts a blob by reference; GC deletes it
  (in-degree 0) before commit; assert the commit takes a **retryable `ABORTED`**, never fatal
  `FILE_DOESNT_EXIST`; and with the blob present-but-condemned it resurrects and commits.
- **Order invariant:** with a counting/recording backend, assert **zero** pool ops (no `head`, no
  `putIfAbsentStream`/`putIfAbsent`, no `get`) occur on content-blob keys **before** the precommit
  ref write, for a part with both fresh-written and adopted blobs.
- **adoptEvidence** records the dep with no backend call.
- **Round-trip:** a part with N written + M adopted blobs commits, reads back identical; a re-insert
  of duplicate content dedups (412 path) correctly post-precommit.
- Full CA gtest suite green (modulo the 2 known pre-existing reds); a no-chaos soak segment over the
  hot-duplicate workload shows no fatal `FILE_DOESNT_EXIST` from adopts.

## Files
- `Core/CasBuild.{h,cpp}` — `adoptEvidence` (done); ensure `putBlob` overwrites the pre-recorded
  tokenless dep (verify; it already assigns the dep).
- `ContentAddressedWriteBuffers.{h,cpp}` — content callback records a pending blob and **transfers**
  temp-file ownership instead of uploading+deleting.
- `ContentAddressedTransaction.{h,cpp}` — `PartStaging` gains a pending-blob list (`{hash,temp_path,
  size}`) + temp-file cleanup; the content `writeFile` callback records pending instead of `putBlob`;
  the three adopt sites call `adoptEvidence`; a single ordered publish routine does
  `putTree → precommit → materialize pending blobs → publish`; both `commit()` and the B151
  eager-rename path go through it.
- `Disks/tests/gtest_cas_build.cpp` / `gtest_ca_transaction.cpp` — the order-invariant + B188
  regression tests.
