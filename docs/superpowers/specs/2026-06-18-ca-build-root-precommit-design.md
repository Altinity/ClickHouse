# CA build-root / precommit redesign — B140-dangle real fix

**Status:** approved (brainstorming dialogue, 2026-06-18). Supersedes the fold-only B140-dangle v2 fix.
**Tracking:** B171. **Root-cause report:** `../reports/2026-06-18-ca-b140-dangle-trigger-pinned.md`.

## 1. Problem

B140-dangle = GC deletes a blob a live, in-flight build still intends to publish, so the published
part references a missing object (data loss). Pinned live in the 2026-06-18 soak via the B170 event
log: part `…28118_28624_64` → tree `3715345a4150` → blob `d3e1ba56…` was deleted at GC round 265 and
the build published the referencing tree 29 s later.

**Why the current protection fails.** Protection today is a per-object self-describing hint:
`reuseBlob`/`putBlob` stamp `cas_owner = <server>:<epoch>:<build_seq>` into the object's S3 metadata,
and GC's `protectedByLiveBuild` refuses to condemn an object whose `owner_seq ≥ min_active` of a live
server. Two structural flaws:

1. **Adopt does not transfer ownership.** `reuseBlob`'s adopt arm moves no bytes and rewrites no
   metadata, so an adopted (deduped) object keeps the `cas_owner` of whoever *wrote* it — possibly a
   long-retired build. When that writer retires and `min_active` rises past its seq, protection
   lapses *while a different, still-in-flight build holds the object*. GC deletes it; the adopter's
   later publish dangles.
2. **Protection is a revocable hint.** The owner can revoke it unilaterally simply by finishing
   (commit or abort). Any consumer relying on it — a second replica, or a long local build that
   referenced a *pre-existing* object whose independent refs are dropped mid-build — is exposed to the
   same "revoke vs not-yet-committed-reference" race. This is the general bug; the resurrect/republish
   trace is one instance.

A hint can never be the correctness boundary. The invariant must be enforced where the build holds
its full closure: a durable two-phase commit.

## 2. Design: protection = reachability from a durable build root

A build's intent becomes a **real, GC-understood reference** instead of metadata advice.

- A new **build root** namespace holds **precommit** manifests. It is just another root the GC fold
  walks — anything reachable from it has in-degree ≥ 1 and GC structurally cannot collect it.
- Before relying on any object, a build **precommits**: it publishes its manifest *tree* under the
  build root, referencing every blob by content-hash. This is a tiny write (hashes only); it gives
  every referenced object — freshly-written *or* adopted — a build-root edge.
- New blobs are written straight to the pool (local staging only, as merges already do; S3 staging +
  server-side copy is deferred — see B172). Because the precommit references blobs **by content-hash**,
  it can be published *before* the bytes land; the build-root edge protects each blob from the instant
  it appears.
- The **real commit** publishes the table-namespace ref → tree only after verifying the full closure
  is present (**fail-closed**), then **removes the precommit**. The shared objects pass from
  build-root-protected to table-ref-protected with no in-degree-0 window.
- **`cas_owner` and `protectedByLiveBuild` are deleted.** The per-server watermark survives but is
  **repurposed** from a per-object protection floor to a *liveness signal for reclaiming abandoned
  precommits*.

### Two build flavors

- **Build-by-creation** (new bytes): write blobs to pool → publish precommit tree → real commit
  (fail-closed) → remove precommit. (Order detail in §4: the precommit edge for a blob must exist
  before that blob could otherwise reach in-degree 0; see the ordering rule.)
- **Build-by-adoption** (no new bytes — FREEZE / detached re-attach / replication relink / pure
  dedup of already-committed content): no staging at all; publish a precommit referencing the existing
  pool objects → real commit → remove precommit.

## 3. Invariants

- **INV-NO-DANGLE-COMMITTED** *(strict, reader-facing).* Every object in the transitive closure of any
  **table-namespace** ref is present. This is the only place the strict no-dangling rule applies; it
  is what readers depend on.
- **INV-BUILDROOT-PROTECTS.** A *present* object reachable from a live build's precommit is never
  condemned/deleted by GC.
- **INV-BUILDROOT-RECLAIM.** An abandoned precommit (its owning build is dead) is eventually removed;
  thereafter its exclusively-owned objects become collectable.
- **INV-COMMIT-FAILCLOSED.** The real commit publishes a table ref **iff** the full closure is present
  at the commit CAS; otherwise it aborts (never publishes a dangle) — *even if its precommit was
  prematurely reclaimed*.

The build root is explicitly **not** subject to INV-NO-DANGLE: it legitimately holds edges to
not-yet-uploaded (pending) and already-deleted (aborting) objects. It is an *intent* structure, never
resolved by the read path.

## 4. Mechanics

### 4.1 Build root namespace & precommit

- A reserved namespace, e.g. `roots/_builds/<server_hex>/<build_seq>` (one precommit slot per
  in-flight build; mirrors the existing `roots/<ns>/<shard>` manifest + journal machinery so GC's fold
  walks it unchanged). The precommit ref names the build's manifest tree.
- Publishing the precommit = one `mutateShard`-style CAS on the build-root shard adding the ref +
  journal Add. GC folding the build root turns that into a root edge → tree → blobs, lifting their
  in-degree.

### 4.2 GC fold of the build root — pending-tolerance

- GC folds the build root like any root, contributing in-degree to reachable objects.
- **Pending tolerance:** a build-root tree edge to an *absent* object (not yet uploaded, or already
  reclaimed) is **not** a dangling alarm and triggers **no** delete — there is nothing to delete; the
  edge is a reservation. (Contrast: an absent object reachable from a *table* ref is a violation.)
- A present object with a build-root edge is protected exactly like any in-degree ≥ 1 object.

### 4.3 Reclaim of abandoned precommits (repurposed watermark)

GC, while folding the build root, drops a precommit whose owning build is no longer live, reusing the
existing per-server watermark read (`watermarkOf` + the seq-freshness K=2 liveness verdict):

- **Clean abort** → `Build` retires its `build_seq` (`retireBuildSeq`) → server `min_active` rises past
  it → precommit reclaimable.
- **Crash (hard kill)** → process gone; on restart the server has a new `epoch` → the precommit's
  epoch mismatches → reclaimable.
- **Live build** → `seq ≥ min_active` and `epoch` matches → precommit honored.

Reclaim removes the build-root ref (+ journal Remove); the next fold drops the edges; objects with no
other reference become normal GC candidates.

### 4.4 Commit (fail-closed)

The real commit is one CAS on the table shard that:

1. verifies the full closure of the manifest tree is **present** (the existing `revalidateDeps`/
   `gateCheckDeps` machinery, made **unconditional** here — every dep proven present, re-pin or ABORT,
   no fence-advance gating);
2. on success, sets `refs[name]` + journal Add (the existing publish CAS);
3. then removes the precommit (separate CAS / journal Remove on the build root).

If step 1 finds a missing non-recreatable object (e.g. its precommit was prematurely reclaimed and a
shared blob was then collected), it **aborts and retries** — never commits a dangle. Premature reclaim
costs work, not data.

### 4.5 Ordering rule (the residual window)

The precommit edge for an object must exist before that object can reach in-degree 0 from loss of its
*other* references. For a **merge**, the source parts are held by the merge, so no window. For
**ad-hoc dedup** of an unrelated committed blob, the build must publish the precommit (or at least the
edge) before relying on the adopt. The TLA+ model (§6) enumerates the interleavings and the C++ spec
fixes the concrete ordering.

### 4.6 Residual fragility — closed

A live build whose background watermark renewer freezes (e.g. starved by an S3 retry storm) could be
falsely judged dead and have its precommit reclaimed. Closed by: (a) **INV-COMMIT-FAILCLOSED** (commit
re-verifies presence; a reclaimed precommit → abort+retry, never a dangle) and (b) a **reliable
background renewer** on its own thread (independent of the build's S3 work) plus the existing K=2 grace.

## 5. What is removed / changed

- **Removed:** `cas_owner` stamping (`Build::ownerMeta`), `Gc::protectedByLiveBuild`, and the per-round
  `watermark_cache`/`server_live_this_round` *consulted-per-candidate* protection path.
- **Repurposed:** per-server watermark (`CasWatermark`) — now read only to judge precommit liveness for
  reclaim, not per object.
- **Made unconditional:** the publish-time presence verification (`revalidateDeps`) — always runs at
  commit, not only on fence-advance; `gateCheckDeps` folded into it.
- **Added:** build-root namespace + precommit publish/remove (`Build`), build-root fold +
  pending-tolerance + precommit reclaim (`Gc`), build-root layout key (`Layout`).

## 6. TLA+ validation plan

Model two root kinds (table root, build root) over a small object/build space; actions: `WriteBlob`,
`AdoptBlob`, `Precommit`, `Commit` (fail-closed), `RemovePrecommit`, `DropTableRef`, `GcFold`,
`GcReclaimPrecommit`, `GcDelete`, `BuildDie` (heartbeat freeze / crash). Check the 4 invariants.

- **Buggy config** (models today's protection-as-hint, no build root) reproduces an
  INV-NO-DANGLE-COMMITTED violation (adopt-without-protection → delete → commit).
- **Fixed config** (build-root reachability + fail-closed commit + reclaim) holds all four, including
  the **premature-reclaim** interleaving (BuildDie between precommit and commit) and the **ordering**
  interleaving (DropTableRef between adopt and precommit).

## 7. C++ implementation plan (after TLA+ green)

Failing gtest first (reproduce the dangle at unit level), then: build-root layout key; precommit
publish/remove on `Build`; two-phase `publish` with unconditional fail-closed presence check; GC
build-root fold + pending-tolerance + reclaim; delete `cas_owner`/`protectedByLiveBuild`; B170
`CasEvent`s for precommit/commit/reclaim. Build + unit tests green, then 12h soak.

## 8. Deferred

- **B172** — S3 staging area + server-side copy for build-by-creation (streaming merge+upload, hash on
  the fly, then server-side copy). Pure perf/footprint; correctness fix does not depend on it.
- **B167a** clean-shutdown `farewell`, **B167c** drop the redundant per-build `HeartbeatKeeper` once
  full-GC debris is reconciled — both interact with the watermark repurpose; revisit after B171 lands.
