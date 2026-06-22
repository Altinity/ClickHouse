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

---
## Execution log (resumable state)
- **Task 1 DONE+APPROVED** (`21549755d0e`): `get()` NoSuchKey→nullopt.
- **Task 2 DONE+APPROVED** (`d34f65f4587` + cleanup `cee3ae33c91`): `uploadFromSource`; `resurrect` deleted; zero `get()` in CasBuild; 5 protocol tests legitimately rewritten to ABORTED.
- **Task 3 DONE+APPROVED** (`a2752471cc6`): `gateCheckDeps`+`revalidateDeps` → one `checkAndResolveDeps`; behaviorally equivalent, redundant HEADs removed.
- **Task 4 A+B DONE** (`<see git log>`): `adoptStagedBlob` helper (6 sites→1); `republishRef` precommit-first. **REVIEW STILL PENDING.** **Part C DEFERRED** — `createHardLink` committed-source `adoptFromTree`→`readTree` GETs a LIVE (ref-pinned) source tree at staging, before precommit. Not an INV-1 violation (live, not condemned); INV-2 nicety only. Fix needs deferred-tree-entry-read post-precommit OR per-file entry metadata in the ref payload (Store/staging restructure). Track as its own item if not done here.
- **REMAINING: Task 4 review; Task 5** (filter `pending_blobs` by staged-tree hashes — kill orphaned uploads, B189); **Task 6** (retire dead `reuseBlob`/`body_recreatable`); **Task 7** (build + chaos soak: no Code 499 / no fatal adopt; update backlog B190/B189 → DONE). Baseline now 321 pass / 2 known reds.

## UNATTENDED directive (2026-06-21, operator: "continue unattended")
Finish the plan autonomously, then re-review, then run a 24h soak. Exact remaining sequence:
1. **Task 4 review** (combined spec+quality on commit `8c32979ab0e`): verify adoptStagedBlob preserves copy(hardlink)/move(moveFile/moveDirectory) semantics; republishRef does NO content HEAD/GET/PUT before precommit (tokenless tree-evidence + precommit-first); the 2 new CaWiringPrecommitOrder tests are non-vacuous; full CA suite still 321/2 known reds. Fix any issues via the implementer subagent.
2. **Task 5** (spec §5): in `publishStaging`, after `stageTree`, upload only `pending_blobs` whose hash is referenced by the staged tree (kill orphaned uploads, B189). TDD: write+unlink a pending blob in one txn → its key gets NO PUT. 321/2.
3. **Task 6** (spec §6): retire dead `reuseBlob`/`body_recreatable` from the transaction surface (port/delete CasBuildReuseBlob tests to the new primitives). No regression. 
4. **Task 7**: build clickhouse+unit_tests_dbms clean; full CA suite = 2 known reds only.
5. **Final re-review**: dispatch a holistic code reviewer over the whole consolidation diff (from `86c4cd757a5`..HEAD) checking INV-1/2/3 hold uniformly, no resurrect/get-revival anywhere, DRY achieved. Fix findings.
6. **Backlog**: mark B190 + B189 DONE (precommit-first revival consolidation) and B188-followups subsumed; move to archive; commit. Note Part C (createHardLink committed-source live-tree read) as a remaining INV-2 nicety item if not folded in.
7. **24h soak**: fresh ca-soak cluster on the rebuilt binary; `python3 -m soak.run --seed 20260621 --chaos-seed 20260621 --phase 3 --duration 24h --workers 2 --metrics soak_24h_v3.db` (tracked bg, NO nohup). Set an hourly status cron (watch: zero `Code 499` and zero `cannot reuse`/FILE_DOESNT_EXIST; B174 gc/snap bounded; dangling=0). Report hourly; on completion or failure, root-cause via system.content_addressed_log before reporting.
Baseline at this point: 321 pass / 2 known reds (`CaWiringOps.FreezeViaHardLinksIntoShadow`, `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`).
- **Task 4 APPROVED** (`8c32979ab0e`): copy/move semantics preserved; republishRef precommit-first correct. One stale `republishRef` header comment in ContentAddressedTransaction.h ~L111 (still says "adoptTree") — folded into Task 5 fix.
- **Task 5 DONE** (`46b07615dad`): filter pending_blobs by staged-tree hashes (no orphaned uploads); republishRef comment fixed. 323/2. Per-task review DEFERRED to final holistic re-review.
- **Task 6 DONE** (`5ffea837742`): retired reuseBlob/body_recreatable (260 del); tests ported w/ coverage map; depIsTokened/hasDep kept. 323/2. ALL 6 CODE TASKS DONE. Next: clickhouse build (running), final holistic re-review, backlog B190/B189 DONE, 24h soak.
- **FINAL RE-REVIEW: ✅ READY** (`1224b95ac33`): invariants hold uniformly (0 resurrect, 0 revival get(), 0 reuseBlob); one primitive/gate/adopt-helper; Tasks 5+6 correct; no new smells. clickhouse+unit_tests built clean. 8 stale comments = non-blocking cleanup (deferred). NEXT: backlog B190/B189 DONE + 24h soak.

## SOAK v3 RESULT — FAILED at T+249min on a RESIDUAL (consolidation incomplete) — 2026-06-22
- Regression watch was clean through T+194 (Code 499 = 0, get()→nullopt held). At **T+249min** an INSERT hit `WORKLOAD FAILURE: Code 107 ... blobs/0b/0b343... absent — cannot reuse (caller must upload it) (FILE_DOESNT_EXIST)`. So the consolidation REDUCED but did NOT fully eliminate the fatal dedup-vs-GC path.
- **Exact root cause (C++ stack via docker exec):** `checkAndResolveDeps` (gate) → `observeAndAdmit(3-arg)` (CasBuild.cpp:740 / 808, the tokenless/bodyless-adopt view-hit branch) → 3-arg `observeAndAdmit` HEADs the object → **HEAD-ABSENT** (object fully GC-deleted) → throws **FATAL `FILE_DOESNT_EXIST` "cannot reuse"** at **CasBuild.cpp:213**. The condemned-TOKEN case was converted to retryable ABORTED (line 258, works — Code 236 retries seen succeeding in the log); the HEAD-ABSENT case was the leftover original-B188 throw and stayed FATAL.
- **FIX:** change the 3-arg `observeAndAdmit` HEAD-absent throw (line 213) from fatal `FILE_DOESNT_EXIST` to **retryable `ABORTED`** ("object vanished between adopt and observe; retry — INV-3"). VERIFY every caller treats ABORTED as re-upload/retry: putBlob catches ABORTED (lines 159,185 → uploadFromSource from held bytes); gate bodyless propagates ABORTED (INSERT retries, re-materializes from source); uploadFromSource's observeAndAdmit calls must handle it. ADD a test: gate re-observe of a bodyless tokenless dep whose object is FULLY DELETED (HeadThenDeleteOnceBackend or a delete-the-key backend) → ABORTED, NOT FILE_DOESNT_EXIST. The existing tests covered condemned-token→ABORTED but NOT absent-object→ABORTED — that coverage gap let this through.
- **REOPEN B190** (residual): the gate bodyless-adopt HEAD-absent → fatal. Same family, last uncovered sub-path.
- Other soak signals were HEALTHY: B174 gc/snap bounded (gen~4), replicas converged, ABORTED self-heal working (the condemned-dedup→re-upload-from-source path fired thousands of times benignly). Only this one absent-at-gate sub-path is fatal.

## RESIDUAL FIXED — INV-3 vanish-is-retryable now COMPLETE across gate + revival — 2026-06-22
Two commits closed the soak-v3 residual and its sibling:
- **`ef9c9e9c9e2`** (gate): `gateObserveAndAdmit` wrapper in `checkAndResolveDeps` converts the
  HEAD-absent `FILE_DOESNT_EXIST` → retryable `ABORTED` at the two bodyless-adopt gate sites
  (~CasBuild.cpp:806/876). Closes the exact soak-v3 fatal (`cannot reuse` at the gate). Completeness-reviewed.
- **`a0cbca8fd55`** (sibling): `reviveObserve` wrapper in `uploadFromSource` converts the same
  HEAD-absent throw at its two post-412 re-observe sites (~CasBuild.cpp:422/463) → `ABORTED`, which
  `putBlob`'s bounded retry loop turns into a re-upload from held source bytes. Tree callers
  (`uploadStagedTree`/`recreateTree`) get retryable instead of fatal too. TDD test
  `CasBuild.PutBlobVanishDuringRevivalReUploadsNotFatal` (RED→GREEN). 321/2 known reds.
- **Completeness audit (full class closed):** the bare 3-arg `observeAndAdmit` (the only overload that
  throws `FILE_DOESNT_EXIST`) has exactly THREE reachable call sites: `reviveObserve` (353, →ABORTED),
  `gateObserveAndAdmit` (806/876, →ABORTED), and `adoptTree` (550, intentionally fail-closed at staging).
  Every other site (152/429/468/829/906) uses the 4-arg overload (present `HeadResult` in hand; condemned
  → ABORTED, never FILE_DOESNT_EXIST). The dedup-vs-GC fatal class is uniformly closed by construction.
- **NEXT: 24h SOAK v4** on the fully-fixed binary (both commits). Watch: zero `Code 499`, zero
  `cannot reuse`/fatal `FILE_DOESNT_EXIST`; B174 gc/snap bounded; replicas converged; dangling=0.
