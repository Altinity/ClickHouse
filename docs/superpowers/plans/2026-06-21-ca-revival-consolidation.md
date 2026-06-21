# CA revival consolidation — implementation plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development (fresh implementer per task + spec
> review + code-quality review). TDD. Frequent commits. Steps use `- [ ]`.

**Goal:** Enforce the revival-from-source + precommit-first invariants uniformly: no code path reads a
condemned/dying object; one revival primitive; one publish gate; one adopt helper.

**Spec:** `docs/superpowers/specs/2026-06-21-ca-revival-consolidation-design.md` (authoritative — each
task implements a numbered section of it).

**Branch:** `cas-vfs-path-mapping`. CA gtests baseline: 315 pass / 2 known pre-existing reds
(`CaWiringOps.FreezeViaHardLinksIntoShadow`, `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`). Build:
`ninja -C build unit_tests_dbms clickhouse > build/build_<task>.log 2>&1` (subagent-analyze log; no -j).
Run: `./build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_<task>.log 2>&1`.

Tasks are ordered so each leaves the suite green. Tasks 2–3 are coupled (deleting `resurrect` requires
its callers rerouted in the same task).

---

### Task 1 — `get()` honors the optional contract (spec §2)
**Files:** `Core/CasObjectStorageBackend.cpp`; test `Disks/tests/gtest_cas_backend.cpp` (or `gtest_cas_backend_contract.cpp`).
TDD: write a test where the backend's underlying `readObject` throws `S3Exception(NoSuchKey)` mid-GET
(after a successful HEAD) → assert `ObjectStorageBackend::get` returns `std::nullopt`, not an exception.
Then wrap `readObjectRanged` in the Native `get` branch to catch NoSuchKey/404 → return `nullopt`.
Independent, smallest, foundational. Commit.

### Task 2 — `uploadFromSource` primitive; delete `resurrect`; reroute blob/tree revival (spec §1)
**Files:** `Core/CasBuild.{h,cpp}`; test `gtest_cas_build.cpp`.
- Add `Build::uploadFromSource(kind, hash, key, std::string_view source_bytes)` (fresh tag + this
  build_id, `putIfAbsentStream`; on 412 `observeAndAdmit`; never GETs key).
- `putBlob` condemned-dedup branch: call `uploadFromSource(Blob, …, <BlobSource bytes>)` instead of
  `observeAndAdmit→resurrect`. (Thread the source bytes into the branch.)
- `recreateTree` + `uploadStagedTree`: become thin callers of `uploadFromSource(Tree, …, retained_trees[hash])`;
  merge the duplicated tree-upload bodies.
- **Delete `Build::resurrect`.** Any remaining caller (gate) handled in Task 3 — if Task 3 not yet done,
  temporarily route gate's blob/adopted condemned-dep to `throw ABORTED` (the final form anyway).
- TDD with `HeadThenDeleteOnceBackend`: a condemned blob deleted in the HEAD→GET window → `putBlob`
  re-uploads from source cleanly (no 499, no fatal). Commit.

### Task 3 — merge the publish gate (spec §3)
**Files:** `Core/CasBuild.{h,cpp}`; test `gtest_cas_build.cpp`.
Collapse `gateCheckDeps` + `revalidateDeps` into one single-pass `checkAndResolveDep` routine. Per dep:
retained tree → `uploadFromSource` (recreate); blob / adopted-tree with no source → retryable `ABORTED`;
present-matching → keep; present-token-changed → re-observe via the 4-arg `observeAndAdmit` (pass the
held `HeadResult`; drop the redundant second HEAD). No revival read. Preserve the existing publish
fail-closed semantics. TDD: the existing gate tests + a condemned-adopted-blob-at-gate → ABORTED. Commit.

### Task 4 — `adoptStagedBlob` helper + precommit-first at remaining sites (spec §4)
**Files:** `ContentAddressedTransaction.{h,cpp}`; tests `gtest_ca_wiring.cpp`.
- Add `adoptStagedBlob(pb, entry, dst_st, dst_build, move_pending)`; replace the 6 inline pending-vs-
  committed blocks in `createHardLink`/`moveFile`/`moveDirectory`.
- `republishRef`: tokenless tree-evidence dep + `precommit` first (drop the pre-precommit `adoptTree` HEAD).
- `createHardLink` committed-source: no `readTree`-GET before precommit (record evidence dep from ref
  metadata; defer any genuine source-tree read to post-precommit or fail retryable).
- TDD: extend `CaWiringPrecommitOrder` so `republishRef` + committed-source `createHardLink` do no
  content HEAD/GET/PUT before the precommit write. Commit.

### Task 5 — filter `pending_blobs` by staged-tree hashes (spec §5)
**Files:** `ContentAddressedTransaction.cpp`; test `gtest_ca_wiring.cpp`.
In `publishStaging`, after `stageTree`, upload only `pending_blobs` whose hash is referenced by the
tree. Remove the unlink/replace orphaned-pending comments. TDD: write+unlink a pending blob in one txn
→ commit → that blob is NOT uploaded (recording backend sees no PUT for its key). Commit.

### Task 6 — retire dead `reuseBlob` / `body_recreatable` (spec §6)
**Files:** `Core/CasBuild.{h,cpp}`; tests.
Remove `reuseBlob` + `body_recreatable` from the transaction surface (port/delete the `CasBuildReuseBlob`
tests to the new primitives). If a two-build-merge downgrade removal is in scope, do it; else leave a
note (no regression). Commit.

### Task 7 — build + full suite + soak
Build clickhouse+unit_tests_dbms clean; full CA suite = 2 known reds only; then restart the ca-soak
cluster and run a chaos soak confirming no `Code 499` and no fatal adopt failures under the
hot-duplicate workload. Update backlog (B190/B189 → DONE), commit.
