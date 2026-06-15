# CA: drop the post-write HEAD — record the PUT-response ETag (design)

**Date:** 2026-06-15. **Branch:** `cas-mergetree-poc`. **Status:** design approved (brainstorming), proceeding to plan.

## Problem

Every Native conditional write in the CA backend does a follow-up `HEAD` to recover the object's ETag/token, because "the conditional-write plumbing does not return the ETag" (`CasObjectStorageBackend.cpp:147`). Instrumentation (findings doc, run 20260616) showed this **head-after-put is ~73% of ALL S3 HEADs** (`CasDbgMetaHit` ≈ `S3WriteRequestsCount`), on the blob/tree write path *and* the `roots/` `casPut` path. It is pure overhead: the PUT / `CompleteMultipartUpload` response already carries the object ETag.

## Model grounding (`CaIncarnationCore.tla`)

- The recorded token is a **transient, writer-local publish-gate dependency** (`wDeps`); persisted manifest refs are **hash-only** (`INV_NO_DANGLE` comment: "any current incarnation satisfies a reader"). So this change touches only the transient gate token, never persisted state or the reader contract.
- `WCreate` records the token the writer **wrote** (`nextTok[h]`) — the model **never** does a head-after-put. `WReuse` (dedup hit) reads the *current* token (`tokOf[h]`) — a genuine observation.
- Therefore: recording the **PUT-response ETag** (our incarnation) is exactly `WCreate`; the head-after-put is an implementation artifact. It is also *more correct* than head-after-put, which reads the *current* token and can observe a racing displacement on `casPut`/`putOverwrite` (head-after-put → a different incarnation than the one we committed).
- The token is **load-bearing for the gate** (`SabotageNoReobserve` MUST dangle; tokenless evidence is safe only for already-reachable objects) — so we drop the *HEAD*, never the *token*.

## Architecture

Thread the final object ETag from the conditional write up to the CA backend, which records it as the dependency token in place of the post-write HEAD. Three layers:

### 1. `WriteBufferFromS3` (`src/IO/WriteBufferFromS3.{h,cpp}`)
Capture the **final object** ETag after a successful upload:
- new member `std::optional<String> object_etag;`
- set it from `outcome.GetResult().GetETag()` in `makeSinglepartUpload` (the `PutObject` outcome) and in `completeMultipartUpload` (the `CompleteMultipartUpload` outcome — the final object ETag, distinct from the per-part `part_tag` already captured at line ~612).
- valid only after `finalize()` completes successfully.

### 2. Generic accessor on the write-buffer base
`WriteBufferFromFileBase` (the type the CA backend holds from `object_storage->writeObject`) gets:
```cpp
virtual std::optional<String> getResultObjectETag() const { return {}; }
```
overridden in `WriteBufferFromS3` to return `object_etag`. Keeps the CA backend off an S3 `dynamic_cast`. Default-empty base means every other write-buffer is unaffected.

### 3. CA backend (`CasObjectStorageBackend.cpp`)
Replace both head-after-put sites:
- `nativeConditionalPut` (serves `putIfAbsent`, `putOverwrite`, `casPut`): on `finalizeConditionalWrite == Done` with `out_token`, set `*out_token = Token{*buf->getResultObjectETag(), TokenType::ETag}` instead of `backend.head(key)`.
- `NativeStreamingSink::finalize` (serves `putIfAbsentStream`, the blob path): same, from `write_buf->getResultObjectETag()`.
- The **`PreconditionFailed` path is unchanged** — that is the dedup-reuse case; its token comes from `observeAndAdmit` (model `WReuse`), a genuine observation of an object we did not write. Its HEAD stays.

## Data flow
`PutObject`/`CompleteMultipartUpload` response ETag → `WriteBufferFromS3.object_etag` → `getResultObjectETag()` → CA backend `Token{etag, TokenType::ETag}` → recorded `wDeps`/`DepEntry` token (= model `WCreate` `nextTok`).

## Capability, not fail-close (design refinement, 2026-06-15)
The approved design said "fail-loud if the ETag is absent." Refined after checking the code: Native mode is **not** S3-only — it also runs over `LocalObjectStorage` (tests, local-disk CA), whose "etag" is a file-mtime computed at HEAD time (`LocalObjectStorage.cpp:379`), not produced by its write buffer. So `getResultObjectETag()` is a **capability**: `WriteBufferFromS3` returns the captured strong ETag; every other write buffer returns `nullopt`. The CA backend therefore uses **the write ETag when the buffer provides one (S3 → no HEAD), else the existing `head(key)` (Local → a cheap local stat)**. This is *not* a bug-hiding fallback: a backend that genuinely has no write-time ETag (Local) legitimately needs the HEAD, and that HEAD is correct + cheap there. The S3 HEADs — the entire ~73% target — are eliminated. Emulated mode untouched (`TokenType::Emulated`). `PreconditionFailed` needs no ETag.

## Testing
- **Consistency:** the token returned by a conditional write equals the token a subsequent `head(key)` returns (test backend), for `putIfAbsent`, `putOverwrite`, `casPut`, and the streaming sink.
- **Zero HEADs on the `Done` write path:** with the counting test backend, a successful conditional write increments writes and leaves `head` count at 0 (the core regression guard for the fix).
- **Multipart:** a blob large enough to force multipart returns the `CompleteMultipartUpload` ETag (not an empty/part ETag).
- **No semantic regression:** full `CaWiring*` + `Cas*` suites green (recorded token value is unchanged for `putIfAbsent`; for `casPut` it is now our committed incarnation, which is correct/stronger).

## Scope / out of scope
- In: the three Native conditional-write entry points + the streaming sink; the `WriteBufferFromS3` ETag capture + base accessor.
- Out: the dedup-reuse `observeAndAdmit` HEAD (`WReuse`, inherent); the publish-gate revalidation HEAD (rare, stale-view only); any GC change; the diagnostic `CasDbg*` instrumentation (kept on the PoC branch to measure the reduction in the soak).
- No TLA+ model change — the code is converging *to* the model (`WCreate`).
