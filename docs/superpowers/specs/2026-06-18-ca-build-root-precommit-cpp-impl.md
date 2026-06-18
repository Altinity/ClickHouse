# C++ implementation spec — build-root / precommit (B171)

**Status:** draft pending TLA+ green (`CaBuildRootPrecommit`). Derived from
`2026-06-18-ca-build-root-precommit-design.md`. Will be reconciled with any refinement the TLA+ model
surfaces before the writing-plans phase.

**Files (all under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`):**
`Core/CasLayout.h`, `Core/CasStore.{h,cpp}`, `Core/CasBuild.{h,cpp}`, `Core/CasGc.cpp`,
`Core/CasEvent.{h,cpp}`, tests under `src/Disks/tests/`.

## A. The build root namespace

A reserved namespace folded by GC like any root, so protection = ordinary in-degree.

- Convention: build-root namespace string `"_builds/" + server_hex`; one **shard per build** keyed by
  `build_seq` (so each in-flight build owns an isolated shard manifest — no cross-build CAS
  contention). A precommit is a single ref `"part"` (or the part name) → manifest tree in that shard.
- `Layout`: reserve the `_builds` segment in `checkNamespace` (alongside `_files`), and add a helper
  `bool isBuildRootNamespace(const RootNamespace &)` (string starts with `_builds/`). It is a normal
  namespace key-wise (`rootShardKey` works unchanged), so the registry/fence/fold/discovery machinery
  needs no new key shapes — only behavioral branches keyed on `isBuildRootNamespace`.

## B. `Build`: two-phase commit (precommit → fail-closed commit → remove precommit)

`Core/CasBuild.{h,cpp}`.

1. **`void Build::precommit(const TreeId & manifest)`** — publishes the manifest tree under the build
   root: `store->ensureRegistered(buildRootNs())` then `store->mutateShard(buildRootNs(), buildShard(),
   set refs["part"] = {tree_id=manifest,...} + journal Add)`. Records nothing new in `deps` (the deps
   already exist from putBlob/reuseBlob/adoptFromTree). Emits `CasEvent` `Precommit`. Must be called
   **before** the build relies on any adopt being durable (ordering rule, design §4.5) — for the
   merge/INSERT path the call site is right after the manifest tree is assembled (`putTree`), before
   the final commit; the build-root edge then protects every child from that point.
   - Because references are by content-hash, `precommit` may run before all blobs are uploaded; GC
     tolerates absent targets under a build root (§C.2). New blobs are written straight to the pool
     (local staging only).
2. **`Build::publish` (the real commit) becomes fail-closed + precommit-removing:**
   - Make the closure presence-verification **unconditional**: always run `revalidateDeps()` (fold
     `gateCheckDeps` into it / call it directly) at the start of the commit `mutateShard` lambda,
     dropping the `if (view.round() < fence_round)` gate and the "evidence as fresh as the view" skip
     for this final pass. Every dep is proven present (re-pin via resurrect/recreate/observeAndAdmit;
     a missing non-recreatable blob → `ABORTED` → caller retries). This is `INV-COMMIT-FAILCLOSED`.
   - On the successful table-shard CAS, after the lambda returns, **remove the precommit**:
     `store->dropRef(buildRootNs(), "part")` (drops the build-root edge; GC fold then releases it now
     that the table ref pins the closure). Emit `CasEvent`s `BuildPublish` (existing) + a new
     `PrecommitRemoved`.
   - Ordering at commit: table ref Add is committed FIRST (closure now table-pinned), THEN precommit
     removed — never the reverse (else a window with neither edge). If `dropRef` of the precommit
     fails transiently, it is safe to leave (GC reclaims it as a stale precommit later); log/emit.
3. **Abort / dtor:** `~Build` / `abandon` removes the precommit if one was published (best-effort;
   GC reclaim is the backstop), in addition to the existing `retireBuildSeq`.

## C. `Gc`: fold the build root, pending-tolerance, reclaim

`Core/CasGc.cpp`.

1. **Fold:** the build-root namespace is discovered via the registry like any namespace (`Build::
   precommit` calls `ensureRegistered`). `foldShardRecords` processes its journal → root edges → tree
   expansion → in-degree, unchanged. Result: every object reachable from a live precommit has
   in-degree ≥ 1 and is never a zero-in-degree candidate. **This replaces `protectedByLiveBuild`.**
2. **Pending-tolerance:** the existing fold path that fails closed on a live ref → missing tree (the
   `FailClosed` / `INV-NO-DANGLE` guard) must be **skipped when `isBuildRootNamespace(ns)`** — a
   build-root edge to an absent target is a pending/aborting reservation, not a dangle. Likewise GC
   never tries to delete an absent object referenced only by a build root (there is nothing present to
   delete). Add the namespace-kind branch at the fold's missing-target site.
3. **Precommit reclaim:** while folding a build-root shard, GC decides liveness of the owning build
   from the shard's `server_hex` + `build_seq` (the namespace + shard encode them) against the
   server's watermark (reuse `watermarkOf` + the K=2 seq-freshness verdict): if the server is dead
   (epoch mismatch / farewell) OR `build_seq < min_active` (build retired), the precommit is
   abandoned → GC removes the build-root ref (`mutateShard` drop / journal Remove) so the next fold
   releases its edges. Emit `CasEvent` `PrecommitReclaim`. (This is the watermark **repurposed**:
   per-precommit liveness, not per-object protection.)
4. **Delete `Gc::protectedByLiveBuild`** and its call site (the retire-decision `skip:
   protectedByLiveBuild` branch); delete the per-round `watermark_cache`/`server_live_this_round`
   *protection* use — but KEEP `watermarkOf` + the liveness verdict for §C.3 reclaim. Retire decision
   simplifies to: present ∧ known ∧ inDeg=0 ∧ (not a build-root-protected object — now automatic via
   in-degree) ⇒ condemn.

## D. `Store` / `CasBuild`: delete `cas_owner`

- Delete `Build::ownerMeta()` and all `putIfAbsentStream(key, ownerMeta())` / `putOverwrite(...,
  ownerMeta())` → pass empty/no metadata. New blobs no longer carry owner metadata; protection is the
  precommit edge. (B167b local-disk metadata gap becomes moot for protection.)
- Keep `CasWatermark` and `Store::minActive`/`allocateBuildSeq`/`retireBuildSeq` — now feeding
  precommit reclaim liveness.

## E. `CasEvent` taxonomy additions

`Core/CasEvent.{h,cpp}`: add `Precommit`, `PrecommitRemoved`, `PrecommitReclaim` to `CasEventType` +
`toString`. Emit at the §B/§C sites with non-empty `reason` (completeness mandate). Remove no existing
types.

## F. Tests (failing-gtest-first)

1. **`CasBuildRootDangle.SharedBlobSurvivesSourceDropDuringBuild`** (RED first): build Q adopts a blob
   shared with committed part P; drop P's ref; run GC to fixpoint; Q commits. Assert the committed
   ref's closure is fully present (no dangle). Without the fix this reproduces the delete-then-commit
   dangle; with it, the precommit keeps the blob alive.
2. **`CasBuildRootDangle.PrematureReclaimCommitFailsClosed`**: force a precommit reclaim (simulate dead
   build / advance min_active) mid-build, delete the now-unprotected blob, then commit → must `ABORTED`
   (retry), never publish a dangle.
3. **`CasBuildRoot.PrecommitProtectsAcrossGcRounds`**: precommit, run several GC rounds while the only
   table reference is absent/dropped → blob never deleted.
4. **`CasBuildRoot.AbandonedPrecommitReclaimed`**: publish a precommit, retire the build (min_active
   advances) → GC reclaims the precommit → the exclusively-owned blob becomes collectable.
5. Keep all existing gtests green: `CasGc*`, `CasBuild*`, `CasReuseGcRace.*`, `CasEvent.*`,
   `CasGcRetire.*`. Update any that asserted on `protectedByLiveBuild` / `cas_owner` to the new
   reachability model.

## G. Out of scope / deferred

S3 staging + server-side copy (**B172**). `farewell` clean-shutdown (**B167a**), drop redundant
per-build `HeartbeatKeeper` (**B167c**) — revisit after this lands.
