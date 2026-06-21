# CA revival consolidation — design (B190 + B189 + resurrect-invariant)

**Status:** design for review
**Date:** 2026-06-21
**Supersedes the narrow B190 read-side framing.** Backlog: B190 (resurrect-by-GET), B189
(orphaned pending / two-build downgrade). Memory: [[feedback-ca-resurrect-invariant]].

## Invariants (operator-mandated)
- **INV-1 (revival-from-source):** a condemned / being-deleted / vanished object is NEVER read/GET-ed
  to revive it. Revival = a fresh re-upload from the writer's OWN source bytes (private staging /
  local temp), or recreation from a RETAINED payload (trees). Reading the dying object is forbidden.
- **INV-2 (precommit-first):** the build-root precommit (full manifest tree by locally-known hashes)
  is published BEFORE any pool upload/HEAD/GET of the build's content; every (re)created incarnation
  is thereby protected.
- **INV-3 (benign race):** any race with a GC delete resolves by recreating a fresh incarnation from
  source (already precommit-protected) — never a fatal error, never a read of the old object.

## Why now / what the audit found
The single revival-read of a condemned object is `Build::resurrect`'s `get()` (`CasBuild.cpp:389`).
It is reached from `putBlob` (holds source bytes), `uploadStagedTree` (holds retained payload),
bodyless `reuseBlob`/`adoptTree`, and the publish gate (no source). `ObjectStorageBackend::get` does
HEAD-then-GET, so a delete in the window throws a raw S3 `Code 499` that bypasses the `nullopt`
contract; `InMemoryBackend::get` is atomic (no window), which is why unit tests can't reproduce it.
Around this sit several near-duplicate code paths (gate, tree-upload, three adopt sites) and dead
APIs (`reuseBlob`/`body_recreatable`). Consolidating them enforces the invariants uniformly with a
minimal set of cases.

## Design

### 1. One revival primitive; delete `resurrect`
Add `Build::uploadFromSource(ObjectKind kind, const UInt128 & hash, const String & key, std::string_view source_bytes)`:
- Encode an envelope with a FRESH `incarnation_tag` + this build's `build_id`, body = `source_bytes`,
  `putIfAbsentStream` (object absent ⇒ created fresh) — on `PreconditionFailed` (a live incarnation
  exists) `observeAndAdmit` (adopt the live token, or if itself condemned and we still hold source,
  re-PUT via `putOverwrite(If-Match)` once more — bounded). Records the tokened dep. **Never GETs `key`.**
- **`Build::resurrect` is deleted.** Its callers become:
  - `putBlob` condemned-dedup: `uploadFromSource(Blob, hash, key, <the BlobSource bytes it holds>)`.
    (Content-addressed: the writer's bytes are byte-identical to the condemned object — no GET needed.)
  - `recreateTree` / `uploadStagedTree`: a thin call to `uploadFromSource(Tree, hash, key, retained_trees[hash])`.
    Merge the duplicated tree-upload bodies into this one path.
  - Publish gate (see §3) and bodyless adopt: **no source ⇒ throw retryable `ABORTED`** (the outer
    operation re-runs and re-uploads from its real source). Never resurrect-by-GET.

`putBlob` must make its source bytes available to the condemned-dedup branch (today the `BlobSource`
is in scope in `putBlob`; thread it into the dedup branch instead of calling `observeAndAdmit→resurrect`).

### 2. `get()` honors the optional contract (defensive, backstops legitimate reads)
In `ObjectStorageBackend::get` (Native branch), wrap `readObjectRanged` so a concurrent-delete
`S3Exception(NoSuchKey/404)` *during the GET* returns `std::nullopt` (object vanished between HEAD
and GET = absent), matching `InMemoryBackend`. This removes the raw-499-escape class for ALL callers.
Revival no longer GETs anyway; this makes legitimate live reads (`Store::readTree` for SELECT/merge,
`loadShardDecoded` already documents this) surface a clean `FILE_DOESNT_EXIST`/empty instead of a raw
499. (The deeper "GC vs a live reader holding a dropped part" lifecycle question is OUT OF SCOPE here
— this fix only makes it a clean handled error, not a raw 499.)

### 3. Merge the publish gate
`gateCheckDeps` and `revalidateDeps` are near-duplicate loops over `deps`. Collapse into one
`checkAndResolveDep`/single-pass routine. Per dep: tree with retained payload → `uploadFromSource`
(recreate); blob or adopted-tree with no source → retryable `ABORTED`; present-and-matching → keep;
present-but-token-changed → re-observe (pass the already-fetched `HeadResult` to the 4-arg
`observeAndAdmit`, eliminating the redundant second HEAD at the two stale-branch sites). No revival
read anywhere.

### 4. One staging-adopt helper; precommit-first at the remaining sites
- Add a transaction helper `adoptStagedBlob(const PendingBlob * pb, const TreeEntry & entry, PartStaging & dst_st, Build & dst_build, bool move_pending)`:
  pending ⇒ copy (hardlink) or move (moveFile/moveDirectory) the `PendingBlob` record + `recordPendingBlobDep`;
  uploaded/committed ⇒ `adoptEvidence`. Replace the 6 inline blocks in `createHardLink`/`moveFile`/`moveDirectory`.
- `republishRef`: replace `adoptTree` (HEAD before precommit) with a tokenless tree-evidence dep +
  `precommit` first; the merged gate (§3) re-proves at publish. No pre-precommit HEAD.
- `createHardLink` committed-source branch: do NOT `readTree`-GET before precommit. Record the
  tokenless evidence dep from the source ref's metadata; if entry discovery genuinely needs the source
  tree, defer that read to post-precommit (under protection) or fail retryable. No pre-precommit GET.

### 5. Filter `pending_blobs` by the staged tree's hashes
In `publishStaging`, after `stageTree`, compute the set of blob hashes actually referenced by the
tree and upload ONLY those `pending_blobs`. Kills the orphaned-upload (B189) and removes the
unlink/replace special-case comments.

### 6. Retire dead code
`reuseBlob` + `body_recreatable` have no transaction-layer callers post-B188. Remove `reuseBlob` from
the transaction surface (keep only if a test needs it; prefer deleting and porting tests to the new
primitives). Remove the two-build-merge tokened→tokenless downgrade by not having two Builds for one
destination where avoidable (if out of scope, leave a note — do not regress).

## Testing
- **Reproduce the HEAD→GET delete window** with `HeadThenDeleteOnceBackend` (already in
  `gtest_cas_build.cpp`): a `putBlob`/gate operation whose object is deleted between HEAD and GET must
  yield a clean re-upload (putBlob: from source) or retryable `ABORTED` (gate, no source) — NEVER a
  raw `Code 499` or fatal `FILE_DOESNT_EXIST`. This is the test the InMemory backend could not provide.
- **`get()` NoSuchKey→nullopt:** a unit test over a backend that throws NoSuchKey mid-GET → `get`
  returns `nullopt`, not an exception.
- **Order invariant** (existing `CaWiringPrecommitOrder`): extend so `republishRef` and the
  committed-source `createHardLink` also do no HEAD/GET/PUT of content before the precommit write.
- **`uploadFromSource` parity:** a condemned blob re-uploaded from source produces a fresh incarnation
  (new tag), adoptable; a tree recreated from `retained_trees` likewise.
- Full CA gtest suite green (2 known pre-existing reds). Then a chaos soak to confirm no `Code 499` /
  no fatal adopt failures under the hot-duplicate workload.

## Files
- `Core/CasBuild.{h,cpp}` — add `uploadFromSource`; delete `resurrect`; `recreateTree`/`uploadStagedTree`
  thin callers; merge `gateCheckDeps`+`revalidateDeps`; `putBlob` condemned-dedup re-uploads from its
  `BlobSource`; remove `reuseBlob`/`body_recreatable` (or minimize).
- `Core/CasObjectStorageBackend.cpp` — `get()` Native branch catches NoSuchKey→nullopt.
- `ContentAddressedTransaction.{h,cpp}` — `adoptStagedBlob` helper (replace 6 blocks);
  `republishRef` precommit-first; `createHardLink` committed-source no pre-precommit GET;
  `publishStaging` filter `pending_blobs` by staged-tree hashes.
- `Disks/tests/gtest_cas_build.cpp` / `gtest_ca_wiring.cpp` — the tests above.

## Out of scope
- The "GC reclaims a blob a live SELECT/merge reader still holds (dropped part)" lifecycle mismatch —
  separate item; this design only makes its symptom a clean error, not a raw 499.
- S3 private-staging + server-side copy (B172) — location of the source bytes is orthogonal; local
  temp staging is used.
