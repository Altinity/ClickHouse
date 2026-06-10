---
description: 'Authoritative design specification for the content-addressed (CA) MergeTree storage backend — a Merkle DAG of immutable folders reclaimed by a Keeper-coordinated, S3-durable Epoch-Based-Reclamation GC. Narrative design plus a TLA+-convertible formal appendix (state, guarded actions, invariants).'
sidebar_label: 'CA Merkle store design spec'
sidebar_position: 1
slug: /superpowers/specs/ca-merkle-store-design
title: 'Content-Addressed MergeTree Storage — Design Specification'
doc_type: 'guide'
---

# Content-Addressed MergeTree Storage — Design Specification {#ca-merkle-store-design}

**Status:** SUPERSEDED by `2026-06-10-ca-incarnation-store-design.md` (the incarnation-token core: one key per
hash, backend-native exact-token deletes, CAS root manifests with embedded journal, Keeper optional). Kept for
the historical record of the EBR/epoch/generation design and its review trail. Decisions D1–D6 below are no
longer in force where the new spec amends them (notably D1 Keeper-required is reversed and D2 generations are
replaced by incarnation tokens).

Original status: authoritative, single-source. Supersedes the exploration docs in `docs/superpowers/reports/`
(moved to `reports/obsolete/`). Decisions D1–D6 are settled (§9). This document is written to be converted to a
TLA+ model and model-checked (the formal appendix, §A); a separate, later effort plans the phased refactoring
of the current implementation onto this design.

This is the TARGET design. Migration from the current implementation is out of scope here (planned separately).

## Table of contents {#toc}
- [1. Overview and goals](#overview)
- [2. Data model — the Merkle DAG of immutable folders](#data-model)
- [3. On-storage layout](#layout)
- [4. Protocols](#protocols)
- [5. Invariants and the safety argument](#invariants)
- [6. The hinges (assumptions correctness rests on)](#hinges)
- [7. Failure handling](#failures)
- [8. Scale and performance](#scale)
- [9. Decisions (D1–D6) and parameters](#decisions)
- [10. Verification scope](#verification)
- [11. Open items and operational concerns](#open-items)
- [Appendix A. Formal model (TLA+-convertible)](#appendix)

---

## 1. Overview and goals {#overview}

The content-addressed (CA) backend is a `metadata_type = content_addressed` for an object-storage disk. A plain
non-replicated `MergeTree` on such a disk needs no engine or DDL change. The on-store model is **"Git for
MergeTree": a Merkle DAG of immutable folders.** Files are content-addressed blobs; folders (including every
MergeTree part) are content-addressed trees; the only mutable objects are **refs**, which are the GC roots and
the commit points. Reclamation is a **lock-free, Keeper-coordinated, S3-durable Epoch-Based-Reclamation (EBR)
garbage collector**: writers and the background GC never share a mutex; a live writer holds reclamation back
(quiescence) rather than ever losing data.

**Goals.**
- **G1 — non-blocking:** writers do not block each other or the GC on the commit path.
- **G2 — ABA-safe reclamation:** a delete can never collide with a concurrent re-create of the same content.
- **G3 — no full-bucket scan on the hot path:** the GC works from a sharded reverse-index fold (`O(delta)`),
  not a `LIST` of the whole store. A full DAG scan exists only for rare reconcile/rebuild.
- **G4 / `INV-S3-COMPLETE` — S3 is the sole durable truth:** every durable fact lives in S3 and is
  self-describing. Keeper holds only `O(active-writers)` ephemeral coordination; losing Keeper entirely (even
  wiping it) loses no durable state.

This replaces zero-copy replication: `blobs/`+`trees/` are a global per-pool content pool, a ref is a pointer,
and a second server "fetches" a part by publishing a ref to an already-present tree — no byte copy.

**Keeper load is flat in pool size.** The commit hot path is *upload the bytes + two constant CA objects (one
tree, one ref) + a coalesced edge-delta*, with **no per-object and no data-proportional Keeper traffic** —
Keeper holds only `O(active-writers)` ephemeral coordination. This is the decisive contrast with zero-copy
replication, whose per-part/per-blob lock state in Keeper scales with the data and is what melts Keeper on large
clusters.

## 2. Data model — the Merkle DAG of immutable folders {#data-model}

Three object kinds; everything is built from them.

- **Blob** — opaque immutable bytes (one file's content). Identity `H = cityHash128(bytes)` (D5: the same hash
  MergeTree already stores per file in `checksums.txt`).
- **Tree** — an immutable folder: a **canonically serialized**, name-sorted list of entries
  `(name, kind ∈ {blob, tree}, child_hash, child_gen, size, attrs)`, where `size` and `attrs` are **content-derived,
  deterministic** basic file attributes (byte size, and only attributes that are a pure function of content —
  **never** `mtime`/`uid` or any nondeterministic metadata, which would break dedup). Identity
  `T = SipHash-128(serialized entries)`. A tree's hash commits to every descendant **and** to these attributes (the
  Merkle property), so a folder listing / `stat` is served from the tree alone — no per-blob `HEAD`.
- **Ref** — the only mutable object: a named pointer `name → (node_hash, gen, header, commit_epoch)`. Refs are
  the GC roots and the commit points (written last, removed first). `commit_epoch` is the writer's pin epoch in
  which this ref's root-edge `+` was logged — it lets reconcile attribute the root edge to an epoch (so a
  prefix rebuild through `Q` counts only refs with `commit_epoch ≤ Q`).

**Node** = a blob or a tree. **Edge** = a reference from a ref to a node (`ref → (T,g)`) or from a tree to a
child (`(T,g) → (child,g)`). The store is therefore a **Merkle DAG, acyclic by construction** (a parent's hash
needs its children's hashes to exist first), so **per-node reference counting is complete** — a node with no
incoming edge is genuinely unreachable; no cycle collector is ever needed.

> **Terminology — `in-degree` = refcount.** Throughout this doc, a node's **in-degree** is the number of edges
> pointing *into* it — i.e. its **reference count** (`in-degree == 0` ⇔ refcount 0 ⇔ unreferenced ⇔ collectable).
> The one nuance vs a classic `refcount`: it is **not a stored integer mutated in place** — it is *computed* by
> folding the edge-delta log (`gc/log`) and rebuilt by reconcile. So "in-degree" emphasises "derived from edges,"
> but mentally you may read it as "refcount" everywhere.

**Generations (D2).** A node key carries a generation `g`: a **small per-node resurrection counter**, `0` until
the GC retires generation `g` and a writer needs that content again and recreates it above the **durable per-hash floor** derived from `gc/retired` (= 1 + max retired gen; #3). `g` is **decoupled from the epoch**; it exists only
so a GC delete of `(N,g)` and a concurrent re-create can never collide on a key (the re-create goes to a gen
`≥ floor`, a different key — G2/ABA). The floor is durable so this holds even when a reclaim is delayed far past
the recent-retired window.

**Canonical serialization.** Trees use a fixed encoding: entries sorted by `name`, fixed field layout, explicit
`kind`, no nondeterministic fields (no timestamps/uids). Identical folders therefore hash identically — this is
what makes recursive dedup work. (Git's tree format is the template.)

**MergeTree maps onto this as a corner case:**

| MergeTree concept | Representation |
|---|---|
| part file (`*.bin`, `*.mrk`, `primary.idx`, `columns.txt`) | **blob** |
| a part | **tree** of its files; `part_id ≡ T` |
| projections / nested per-part dirs | **subtrees** (recursive, no special case) |
| table active set | **flat refs**, one per part name (D3) |
| merge / mutation carry-forward | a **new tree reusing unchanged children's hashes** (structural sharing) |
| dedup | **recursive**: equal file → same blob, equal folder → same tree, equal part → same tree |
| FREEZE / shadow, BACKUP | a **ref** pinning a tree, table-lifetime-independent |

**D5 — trust `checksums.txt` for part-trees.** Because the blob hash is `cityHash128` (= the part's own per-file
checksum), building a part-tree reads `(name → (cityHash128, size))` directly from `checksums.txt` with **no
re-read or re-hash** of file bytes. So **copy-part / fetch / ATTACH / clone** are near-free: construct the
part-tree from `checksums.txt`, `setRef` it, and upload only the blobs not already present (dedup by hash).
Fail-safe: if the `checksums.txt` algorithm/format does not match the CA blob-hash definition at attach time,
fall back to re-hashing.

## 3. On-storage layout {#layout}

### 3.1 S3 (all durable state) {#layout-s3}
One pool = one disk root.

```
<pool>/
  blobs/<H[:2]>/<H>/<g>                    immutable FILE bytes — node (H,g); object metadata: birth_epoch (the creating
                                              writer's pin epoch; immutable, retained on createIfAbsent). g = resurrection gen (0 common)
  trees/<T[:2]>/<T>/<g>                     immutable FOLDER — canonical entries [(name, kind, child_hash, child_gen, size, attrs)];
                                              node (T,g); object metadata: birth_epoch
  roots/<server_id>/                        mutable GC ROOTS, mirroring the normal ClickHouse on-disk tree
      store/<uuid[:3]>/<uuid>/
          refs/<part_name>                  LIVE REF -> RefPayload{ T, g, header, commit_epoch } — GC ROOT, commit point
                                              (written last, removed first); commit_epoch = pin epoch of its root-edge `+`
          refs/<part_name>.meta             mutable per-part sidecar files (verbatim)
          refs/detached/<name>              detached ref (root, not in the active set)
          files/<tail>                      table-level verbatim files (format_version.txt, …)
      shadow/<backup_id>/store/<uuid[:3]>/<uuid>/refs/<part_name>   FROZEN ref — GC root, table-lifetime-independent
                                              (same table-level layout as store/ above — mirrors the usual shadow path)
  gc/epoch_state                             ONE durable object { open_epoch, folded_through, leader_fence } — the SOLE epoch
                                              authority, updated under the leader fence. open_epoch = epoch handed to NEW writers;
                                              folded_through = highest epoch folded into the authoritative snapshot; leader_fence
                                              fences S3 epoch_state writes. NO seal: an epoch simply stops being open when open_epoch
                                              advances, but a writer PINNED to it may still append there (GC never folds a live pin).
  gc/log/<epoch>/<shard>/<op_id>            EDGE add/remove deltas keyed by (parent,child) — root edge (ref_name, node), internal edge
                                              ((T,g,name),(child,g)). op_id = hash(build_id, edge, sign): retry within the SAME pinned
                                              epoch reuses the key → idempotent. No cross-epoch reappend ⇒ no cross-epoch dedup. Sharded.
  gc/snap/<epoch>/<shard>                    per-node IN-DEGREE authoritative THROUGH <epoch>; sorted by node key, sharded. Rebuildable.
  gc/retired/<retire_epoch>/<shard>          THE single durable barrier: a per-round manifest { retire_epoch, entries:[{(hash,gen), kind,
                                              child_edges if tree}] }, sharded by hash. ONE source of truth with three jobs:
                                                (1) REUSE BARRIER — (H,g') ∈ retired with g' ≥ g ⇒ a writer must not commit references to (H,g);
                                                (2) DELETE-GRACE reference — retire_epoch (open_epoch at retire) gates physical delete (§4.2);
                                                (3) DELETE-RECOVERY — the tree child_edges let a successor finish a crashed cascade.
                                              An entry is kept until its object is physically deleted, then dropped — so the manifest is
                                              bounded by the pending-reclaim backlog, NOT by all-ever-deleted (no per-object tombstone bloat).
                                              (A FloorIndex — a compacted index "max retired gen per hash" — is an OPTIONAL implementation
                                              cache OVER gc/retired for O(1) reuse-check; rebuildable; must fail-closed / refresh if stale.)
  _pool_meta                                pool identity / format version
```
Notes: all mutable roots live under `roots/<server_id>/` and **mirror the real ClickHouse path** (`store/<uuid>`
for live tables, `shadow/<backup_id>/store/<uuid>` for frozen backups) so the metadata layer maps onto it
directly. There is **no per-object tombstone** and **no separate `floors`/`candidates`/`reclaiming` namespaces** —
the single `gc/retired` manifest (one per round, entries dropped after delete) is the whole barrier. There is
**no `active` hint**: a node's present generation is resolved by a `GET <hash>/g=0` first (the **>90% case** —
`g` stays `0`), and only on `404` a `LIST <hash>/` (a `LIST` costs ≈10× a `GET`, so this is a **cold** path, not
the hot path). Retirement gates *reuse/attachment*, never *reads*. Root edges (`ref→T`) ARE logged (so the
routine fold needs no `LIST` of `refs/`); a crash biases to **over-count (leak), never under-count (loss)**, and
reconcile (§4.5) authoritatively cancels any stale positive over the quiesced prefix. **There is intentionally
no `gc/sealed`** — the live-pin invariant (GC never folds an epoch `≥ safe_epoch`) makes a seal unnecessary, so
there is no late-reappend and no cross-epoch dedup.

### 3.2 Keeper (ephemeral coordination only, `O(active writers)`) {#layout-keeper}
```
/clickhouse/ca/<pool>/
  leader/lock-<seq>     EPHEMERAL-SEQUENTIAL; lowest live seq = GC leader; <seq> IS the monotone fence token
  writers/<session_id>  EPHEMERAL; data = that writer's PIN epoch. The writer logs ALL of a build's edge deltas
                        into this pin epoch and keeps the pin published until the build has a durable outcome
                        (commit or abort). A live writer may keep an old pin even after open_epoch advances.
```
That is all Keeper holds — leader election (with the fence token) and one tiny ephemeral node per live writer.
The `writers/<S>` value is the **quiescence pin**: GC computes `safe_epoch = min(live pins)` and processes only
epochs `< safe_epoch`, so a writer's pin epoch is **never folded while the writer is live** — that single
invariant replaces the whole seal/reappend machinery (the writer just keeps appending to its pin epoch).
**The epoch is NOT in Keeper** (v1 decision): it lives only in S3 `gc/epoch_state`, read **fresh (one S3 `GET`)
at build start** — *not* a process cache, which could hand out an already-folded epoch (a real loss path).
That is one control read per part build (not per file). Nothing in Keeper is authoritative durable state, so a
Keeper wipe stays non-destructive (`INV-S3-COMPLETE`).

## 4. Protocols {#protocols}

### 4.1 Writer — commit part `name` {#proto-write}
1. **Lease + pin.** Hold a live Keeper session `S`. **Read `gc/epoch_state` fresh from S3** (one `GET` per build,
   *not* a process cache — a stale cache could pin to an already-folded epoch, a real loss path). Set
   `pin_epoch := open_epoch`; publish `writers/<S> = pin_epoch`. **All** of this build's edge deltas go into
   `gc/log/<pin_epoch>`, and the pin stays published until a durable outcome (step 6). **Self-fence (D1 hinge):**
   a consequential op may proceed only while `Connected ∧ local_elapsed_since_renew < T_session − margin`; else
   the writer goes **read-only**. (`Disconnected`-detection alone is insufficient — TLC CE-4.)
2. **Reuse barrier.** The durable authority is `gc/retired` (§3.1): `(H,g)` is reusable iff no retired `(H,g')`
   with `g' ≥ g` — equivalently `g ≥ floor_gen(H)` where `floor_gen(H) := 1 + max retired gen for H`, a value
   *derived* from `gc/retired` (cached via the optional FloorIndex; the cache must refresh/fail-closed before a
   commit). It is durable, so safety holds even when reclaim lags far past any recent window.
3. **Build locally.** For each file `f`: `Bf := cityHash128(f)` (D5: from `checksums.txt` for part files); reuse
   present `(Bf,g)` iff `g ≥ floor_gen(Bf)`, else `createIfAbsent blobs/<Bf>/<floor_gen(Bf)>` with `birth_epoch :=
   pin_epoch` (immutable; a `createIfAbsent` that finds an existing object keeps its original `birth_epoch`).
   Recurse subdirs → child trees, same rule.
4. **Tree.** `T := SipHash-128(canonical entries)`; `createIfAbsent trees/<T>/<g>` with `birth_epoch := pin_epoch`.
   (`part_id ≡ T`.)
5. **Edge `+`.** After the complete tentative DAG is built, add edge `+` deltas into `gc/log/<pin_epoch>` — root
   `(ref_name,(T,g))` and every internal `((T,g),child(h,g))` — each with a stable `op_id = hash(build_id, edge,
   sign)`, so a retry **within the same pinned epoch** reuses the key and is idempotent. **No seal check, no
   reappend, no second epoch** — the writer may append to `pin_epoch` even after `open_epoch` advances, because
   the GC never folds `pin_epoch` while this writer's pin is live (§4.2). **Floor re-check + ancestor rebuild
   (P1-3):** before committing, re-read `floor_gen` for every reused child; if any reused `(h,g)` now has
   `g < floor_gen(h)`, the child must resurrect to `floor_gen(h)` — and since tree identity includes `child_gen` (§2),
   the parent's hash and every ancestor up to the ref change, so **abort the whole tentative chain above `h`,
   rebuild it, re-flush, re-check**. Never patch a child in place.
6. **Commit or abort (durable outcome — then advance).**
   - **Commit:** `setRef roots/<server_id>/store/.../refs/<name> = RefPayload{T,g,header, commit_epoch=pin_epoch}`
     — written last (re-check self-fence + the floor re-check immediately before).
   - **Abort:** add `-` deltas (same `op_id` scheme) removing every tentative edge of this build, including the
     root.
   Only **after** commit or abort is durable may the writer **advance its pin** (re-read `gc/epoch_state`, set
   `writers/<S> := open_epoch`). **Advancing the pin on a durable `+` alone is forbidden** — it would release
   quiescence on the build epoch while the tentative root edge is unmatched by a real ref, letting reconcile zero
   the node and GC reclaim it before `setRef`. A crash with neither outcome is covered by session expiry (lease
   drops → build epoch quiesces → reconcile cleans the tentative edges).
7. **Drop.** `removeRef` **first**, then add the root-edge `-` delta in the writer's current pin epoch; keep the
   pin until the `-` is durable. (`removeRef`-before-`-` biases a crash to over-count/leak, never loss.)

A crash before step 6 leaves the part not-live; because **the pin is frozen at the build epoch until the durable
outcome**, quiescence protects the tentative node until it commits or the lease drops. A stale-positive root edge
(crash at 5↛6, or 7 before the `-`) makes the fold *over-count* (leak), **never under-count** (loss) — reconcile
(§4.5) cancels it over the quiesced prefix. A node uploaded but never `+`'d (crash between 3 and 5) carries no
edge; it is protected while in-flight by its `birth_epoch = pin_epoch ≥ safe_epoch` (reconcile excludes
`birth_epoch > Q`), and reclaimed only after the writer's epoch quiesces (§4.5). Over-count only in every case —
the accepted cost is that such debris lingers until the next reconcile.

### 4.2 GC leader — one round (fenced single deleter) {#proto-gc}
Guard every step: I am the lowest-seq child of `leader/` (a **`sync`-ed** read) ∧ `Connected`; else **fail-close
(stop)**. A **new** leader re-folds + re-quiesces under its **own** fence before any delete.
```
R0 ROTATE   fenced PUT gc/epoch_state := { open_epoch: open+1, folded_through, leader_fence: my_fence }.
            Hands NEW writers a new pin. Does NOT seal old epochs against already-pinned writers (no seal).
R1 SAFE     safe_epoch := min(pin epoch) over getChildren(writers/) [SYNC-ED read — a stale read could under-count
            live writers]; if none → open_epoch. Only epochs e < safe_epoch are QUIESCED.
R2 FOLD     choose Q with  folded_through < Q ≤ open_epoch−1  ∧  Q < safe_epoch.  Streaming merge-sort
            gc/log/(folded_through, Q]/<shard> onto gc/snap/<folded_through> → gc/snap/<Q>; IN-DEGREE = count of
            DISTINCT present edges per node (idempotent edge-set). After durable, set folded_through := Q (fenced);
            logs ≤ Q may then be deleted. The in-degree-0 nodes are streamed straight to RETIRE (R3) — no separate
            durable candidate/work-list (a transient in-memory queue is fine; lost → re-derived by the next fold).
R3 RETIRE   for each in-degree-0 node (H,g): durably append it to **gc/retired/<retire_epoch>** (retire_epoch =
            open_epoch now), with kind + (for trees) child_edges. This single manifest IS the reclaim authority:
            it is the writer-facing REUSE barrier (`(H,g') with g'≥g` ⇒ no new commit references (H,g)) AND the
            delete-grace reference. (Retired ≠ dead — the object may still be reused/reachable until the grace +
            final recheck below.)
R4 DELETE   delete (H,g) ∈ gc/retired/<r> only when ALL hold (else leave it; retry later):
              safe_epoch > r   (GRACE — a writer that passed its reuse-check BEFORE this entry was written had its
                                pin ≤ r, so by safe_epoch > r it has committed/aborted/died; a writer that started
                                AFTER sees (H,g) in gc/retired and won't reuse it. This is the fix for the
                                reuse-check-vs-retire race — the barrier is `safe_epoch > r`, NOT `safe_epoch > Q`)
              ∧ FINAL RECHECK: current in-degree (folded through some Q' ≥ r) == 0 AND not reachable from any live
                ref   (a writer that committed during the grace is now folded → in-degree > 0 → spared; the retired
                       entry stays, old gen kept alive by that ref, new reuse goes above it)
              ∧ still-leader(fence).
            If (H,g) is a TREE: FIRST durably append the `-` child-edge deltas (read from the tree object itself —
            still present, so a crash before delete just re-reads it; the gc/retired child_edges are an
            optimization for that recovery), THEN delete the object. AFTER the object is deleted, the gc/retired
            entry MAY be dropped (its barrier is moot — `GET g0→404→LIST` now finds the live gen). DEFERRED
            CASCADE (D4): children reach in-degree 0 and retire in later rounds.
```
**One durable barrier; physical delete is a later retryable consequence.** `gc/retired` is the single source of
truth — the writer-facing reuse barrier *and* the delete-grace reference. The delete gate is `safe_epoch > r`
(the retire round, ≥ any reusing writer's frozen pin) **plus a final in-degree/reachability recheck** — NOT
`safe_epoch > Q` (`Q` is only when in-degree was measured; a live writer past `Q` can still reuse an old `(H,g)`
and reference it in an unfolded epoch). There is **no `+2` limbo, no separate `floors`/`candidates`/`reclaiming`
state** — a FloorIndex, if used for fast reuse-lookup, is a rebuildable cache over `gc/retired`, not a second
authority. No wall clock is consulted on the reclaim path.

### 4.3 Reader {#proto-read}
`GET ref → (T,g)`; `resolveNode(x,g)`: `GET <x>/<g>`; walk tree entries, recurse into child trees, `GET` child
blobs at their exact `(hash,gen)`. **The reader does NOT substitute another generation** for the one a ref/tree
names: a live ref naming a deleted generation is an invariant violation, not something to paper over. On a `404`,
`LIST <x>/` only to **confirm** that exact generation is absent; if it is, **re-read the ref** — if the ref is
gone the part was dropped (read fails cleanly, no live ref names it — not a dangle); if the ref **still names the
missing `(hash,gen)`**, report a **storage exception** (it surfaces a real `INV-NO-DANGLE` violation rather than
hiding it). A present-but-retired node still reads correctly (retirement blocks reuse, not reads).

### 4.4 Recovery — total Keeper loss {#proto-recovery}
```
during:  every writer session drops → READ-ONLY (self-fence); the leader's election znode is gone → GC stops.
         No mutation that could dangle or lose data occurs while Keeper is down.  SAFE PAUSE.
restore (even empty Keeper):
         PURGE any backup-restored ghost znodes — leader/ and writers/ (the epoch is NOT in Keeper, so there is no
         ghost-epoch hazard to purge — a direct benefit of the v1 decision).
         Elect a leader;  read gc/epoch_state (S3);  fenced PUT gc/epoch_state.open_epoch += 1 (fence off
         pre-outage in-flight epochs);  writers reconnect, re-create writers/<S>, read gc/epoch_state fresh, resume.
         If gc/snap integrity is in doubt → RECONCILE (§4.5): rebuild the quiesced prefix from refs/ +
         the physical object LIST (cancels stale positives, discovers debris); the quiescence gate reclaims.
```

### 4.5 Reconcile — quiesced-prefix authoritative rebuild {#proto-reconcile}
The `O(delta)` fold cannot fix two drifts: (a) **never-referenced debris** (objects a crashed build PUT before
any `+`, §4.1) — no edge, invisible to the fold; (b) **stale positives** — a `+` left by a crashed commit/drop,
which makes the fold over-count (a leak). A periodic background **reconcile** (rare — e.g. every few days)
repairs both by **rebuilding a quiesced prefix from the durable roots + a physical `LIST`**, using `commit_epoch`
(on refs) and `birth_epoch` (on objects) to attribute each root/node to an epoch. It is `O(1)`-memory via
streaming sorted merges, and it **never deletes** — it only rewrites the authoritative snapshot/candidates.
```
QP  WATERMARK.  Choose Q with  Q < safe_epoch  ∧  Q ≤ open_epoch−1.  Reconcile MUST NOT rebuild or discard logs
    for epochs ≥ safe_epoch (a live writer — possibly on an old pin — still owns those).
R0  WALK reachability from all roots whose commit_epoch ≤ Q (active refs, refs/detached, shadow/) — the durable,
    written-last AUTHORITY (NOT gc/snap). Expand tree reachability by reading trees; spill reachable edges sorted
    by shard + node key. (Refs with commit_epoch > Q are NOT counted into snap/<Q> — their root `+` is still in a
    log epoch > Q and is folded later when that epoch quiesces. This is why refs carry commit_epoch.)
R1  LIST blobs/ + trees/ sorted by key, reading each object's birth_epoch.
R2  STREAMING MERGE  LIST ⋈ reachability-stream, per shard → REBUILD gc/snap/<Q> with the TRUE in-degree through
    Q. Objects with birth_epoch ≤ Q are rebuilt: unreachable ⇒ in-degree 0 (stale positives die, debris
    surfaces). Objects with birth_epoch > Q are EXCLUDED — a live writer's fresh upload with no edge yet is
    protected by its epoch, not misread as old debris. DISCARD logs ≤ Q only after the rebuilt shards are durable.
R3  Reconcile feeds its in-degree-0 nodes into the SAME RETIRE/DELETE path as the fold (§4.2 R3/R4): append them
    to gc/retired/<retire_epoch=open_epoch>, then delete only when safe_epoch > that round with a final recheck.
    Reconcile itself never deletes; it only makes the snapshot authoritative + appends to gc/retired.
```
**Two explicit timestamps make the prefix rebuild computable.** Without `commit_epoch`/`birth_epoch`, a `refs/`
walk + `LIST` cannot tell which roots/objects belong to epochs ≤ Q — it would either count a post-Q ref into
`snap/<Q>` (double-count when its log epoch is later folded) or treat a live writer's edge-less fresh upload as
old debris (the P1 "fresh upload not protected" loss). The two epochs close both: count a ref iff
`commit_epoch ≤ Q`; rebuild an object iff `birth_epoch ≤ Q`.

**Why only the quiesced prefix.** A live writer (possibly on an old pin) still owns epochs `≥ safe_epoch`;
rebuilding them could zero a tentative-but-uncommitted node and let GC reclaim it before commit. `Q < safe_epoch`
plus the §4.1-step-6 advance gate (pin held until commit/abort) means an epoch falls below `Q` only after its
writers committed (→ reachable, kept) or died (→ orphan, zeroed). **No age-filter** (it would be redundant with
the quiesced prefix and unsafe — a skipped young entry could have had its only protection in a discarded `≤Q`
log). If operators want to reduce work on fresh uploads, they may only **lower `Q`** or skip a shard *without*
discarding that shard's logs. An interrupted reconcile is always safe — un-rebuilt shards keep their prior `snap`.

## 5. Invariants and the safety argument {#invariants}

- **INV-NO-LOSS:** a node `(N,g)` is deleted only when its floor was durably raised past `g` (recording
  `retire_epoch`) **and** `safe_epoch > retire_epoch` **and** a final recheck shows current in-degree 0 ∧ not
  reachable from any live ref **and** the leader holds its fence. The barrier is `safe_epoch > retire_epoch`, NOT
  `safe_epoch > Q`: `Q` is only when in-degree was *measured*, and a live writer in an epoch `> Q` can still
  **reuse** an old `(N,g)` (its floor-recheck saw the old floor) and reference it in an unfolded epoch. Any such
  writer had its pin frozen `≤ retire_epoch` (it passed the recheck before the floor raise), so `safe_epoch >
  retire_epoch` ⇒ it has committed (→ folded, the final recheck spares `(N,g)`) or died (→ truly unreferenced).
  This is the floor-recheck-vs-floor-raise race fix.
- **INV-NO-LIVE-WRITER-DELETION:** nothing — fold, retire, delete, reconcile, or log discard — ever processes
  an epoch `≥ safe_epoch`. A live writer's pinned epoch is therefore never touched; and a physical object with
  `birth_epoch > Q` (a fresh, possibly edge-less upload) is excluded from the reconcile rebuild. So neither a
  tentative committed-or-not node nor a brand-new upload of a live writer can be deleted.
- **INV-NO-DANGLE:** a published ref always resolves to the present `(hash,gen)` it names. The reader does **not**
  substitute another generation; if the named generation is genuinely absent it re-reads the ref and, if the ref
  still names it, raises a storage exception (surfacing the violation rather than hiding it — §4.3).
- **INV-COMMIT-ATOMIC:** a ref is published only after its whole tree DAG and edge `+` set are durable, the floor
  re-check still passes, and the ref records `commit_epoch = pin_epoch` of its root-edge `+` — so reconcile can
  attribute the root edge to exactly that epoch.
- **INV-NO-ABA:** a node once deleted is never returned to present at the **same** key — a re-create uses a gen
  `≥ floor_gen(H)` (D2/#3, the durable per-hash floor, raised *before* any delete), a different key; only
  `create-if-absent` is needed.
- **INV-OVER-COUNT-ONLY:** every failure mode (lost/dup/reordered `+`/`-`, crash, partial cascade, **stale root
  positive** from a crashed commit/drop) biases to over-count (a node kept longer → a leak, reclaimed later),
  never to under-count (loss). The §4.1 orderings (`+`-before-`setRef`, `removeRef`-before-`-`) guarantee this
  direction. The reverse index is a *sloppy candidate filter*, not the delete authority; the authority is
  quiescence + in-degree + fence. Two things the `O(delta)` fold cannot fix — never-referenced debris (no edge)
  and stale positives (a `+` outliving a failed commit) — are corrected by the periodic **reconcile** (§4.5),
  which **authoritatively rebuilds** in-degree from the real `refs/` (so a stale positive recomputes to its true
  value and dies). Deliberate choice (§9): no per-object write-ahead tracking on the hot path, and no
  wall-clock-gated deletion anywhere.
- **INV-S3-COMPLETE:** S3 alone determines and rebuilds the full state; Keeper holds **only ephemeral
  coordination** (leader election + per-writer lease) — no durable state, no caches; total Keeper loss loses
  nothing (the epoch lives in S3 `gc/epoch_state`; writers re-read it on restore).
- **Liveness (no-leak-forever):** every genuinely unreachable node is eventually retired (in-degree 0 fold)
  and, after the limbo + quiescence, reclaimed; deferred cascades drain dead subtrees over successive rounds; a
  stuck writer cannot stall reclamation forever (its lease expires → it is dropped from `safe_epoch`).

## 6. The hinges (assumptions correctness rests on) {#hinges}

1. **Writer self-fence on the Keeper session** (D1): a consequential op (reuse-commit) requires
   `Connected ∧ session-alive-by-local-clock` — fail-stop on `Disconnected`, deadline strictly inside
   `T_session`. This is a session-timeout assumption (no inter-clock skew, because Keeper's session is the
   single arbiter). The leader likewise fail-closes on `Disconnected`, with a `sync`-ed lowest-seq fence
   re-check before every delete.
2. **One pinned epoch + commit/abort-before-advance + quiesced-prefix everything** (the central hinge): a writer
   logs **all** of a build's edge deltas into its single `pin_epoch` and **keeps the pin published until a durable
   outcome** — `setRef` (commit) *or* a durable removal of every tentative edge (explicit abort); never advancing
   the pin on a durable `+` alone (§4.1 step 6). The GC computes `safe_epoch = min(live pins)` and **never folds,
   reconciles, or discards an epoch `≥ safe_epoch`** — so a live writer's pinned epoch (even an old pin it kept
   while `open_epoch` advanced) is always protected, and the writer can keep appending to it with **no seal, no
   reappend, no second epoch, no cross-epoch dedup** (op_id makes a retry within the pin epoch idempotent). The
   epoch is read **fresh from S3 `gc/epoch_state` once per build** (not a process cache — a stale cache could pin
   to an already-folded epoch, a real loss path). The `e+2` limbo is sufficient independent of read lag: a fresh
   read pins to the current `open_epoch` (> `folded_through`, never folded); a writer that keeps an old pin only
   lowers `safe_epoch`, which the gates self-adjust to. Safety comes from quiescence, not from `+2`, not from any
   clock. Reconcile uses `commit_epoch`/`birth_epoch` to rebuild strictly the prefix `Q < safe_epoch` (§4.5).
3. **Fenced single deleter**: only the lowest-seq leader deletes; every delete/epoch-advance is fence-gated.
4. **Durable floor `{floor_gen, retire_epoch}`** (D2/#3): a re-create routes to `≥ floor_gen(H)` (ABA-safe with
   only `create-if-absent`). Crucially, raising `floor_gen` only **blocks new reuse** of `(H,g)`; it does NOT
   make `(H,g)` deletable immediately — a writer that passed its floor-recheck *before* the raise may still be
   committing a reference. Physical delete waits for **`safe_epoch > retire_epoch`** (the open_epoch at the
   raise, ≥ any such writer's frozen pin) plus a final in-degree/reachability recheck. (Fixes the
   floor-recheck-vs-floor-raise race.)

## 7. Failure handling {#failures}

| Fault | Outcome |
|---|---|
| writer crash mid-build / mid-(100GB)-upload | no ref published. A stale root `+` (logged before `setRef`) or never-`+`'d debris both bias to **over-count (leak), never loss** (§4.1 orderings) → corrected by the periodic **reconcile** (§4.5), which authoritatively rebuilds in-degree from real `refs/` (stale positive recomputes to 0; debris discovered by the `LIST`) → the usual quiescence + in-degree + fence gate reclaims. Incomplete multipart objects are reclaimed by an S3 lifecycle abort-incomplete rule (infra). Over-count only. |
| writer paused (alive lease) | its frozen pin epoch holds `safe_epoch` down; reclamation of newer epochs waits. Liveness only. |
| writer server-expired but still believes Connected | the §6.1 local-deadline self-fence forces read-only *before* expiry. Safe. |
| GC leader crash mid-round / mid-cascade | idempotent (`gc/retired` is append/drop-keyed, monotone epoch PUT, fenced deletes; re-fold recovers a partial cascade); successor takes a higher fence and re-folds. |
| split-brain leaders | only the lowest-seq leader's `sync`-ed fence passes; the stale leader fails-close. |
| total Keeper loss / restore | §4.4: safe pause → S3-only authoritative rebuild via reconcile (§4.5). No ghost-epoch hazard (epoch not in Keeper). INV-S3-COMPLETE. |
| lost/torn `gc/snap` | authoritatively rebuilt by reconcile (§4.5) from real `refs/` reachability over the quiesced prefix — `snap/<Q>` becomes the new truth; over-protective. |
| stale root positive (crashed commit/drop, §4.1) | fold over-counts → node leaks (over-count, safe); reconcile recomputes in-degree from real `refs/` → stale positive dies. Leak until next reconcile, never loss. |
| live writer's fresh upload, no edge logged yet | protected by `birth_epoch = pin_epoch ≥ safe_epoch`: reconcile excludes `birth_epoch > Q`, so it is never zeroed/reclaimed while the writer's epoch is non-quiesced. After the writer commits, its edges fold normally; if it dies, the upload is debris reclaimed once its epoch quiesces. |

## 8. Scale and performance {#scale}

- **Snapshot fold is `O(delta)`**: `gc/snap` is sharded by hash prefix; a round folds only the touched shards
  via streaming merge-sort. The only `O(all-nodes)` pass is reconcile (off-hot-path, rare).
- **Write amplification**: `+`/`-` edge deltas are coalesced (group-commit, one log object per `(shard,
  window)`); there is **no per-object hot-path write** for orphan tracking (§9). One fresh `GET gc/epoch_state`
  per build (not per file). `birth_epoch` is set inline on the object `PUT` we already do (no extra request).
- **Keeper load**: `O(active writers)` tiny znodes + heartbeats; independent of pool size.
- **Big folders**: parts are small trees. A table keeps **flat refs** (D3), not one giant tree; if a single
  huge directory is ever required, shard it into a trie of trees by name-prefix.
- **Read cost**: one `GET` per node in the common `g=0` case; a node ever resurrected (`g>0`) pays
  `GET`-`404`-then-`LIST` on every read until generations are compacted (open item, §11). The part-load path must
  not trip the cold `404→LIST` en masse (§11).
- **Reconcile cost**: the periodic prefix rebuild (§4.5) is `O(all objects)` but **`O(1)`-memory** — it streams
  the `LIST` and the `refs/`-reachability spill (both sorted by node key) through the merge-sort, never
  materializing a reachability set. Reading live trees during the walk is bounded by part-count (`≪` blobs).
  Reading each object's `birth_epoch` during the `LIST` may cost a `HEAD` per object if it is user-metadata (S3
  `LIST` does not return user metadata) — an implementation open item (§11): store `birth_epoch` where the
  `LIST`/scan can read it cheaply, or accept the `HEAD` cost in the rare reconcile. Schedule + per-pass `LIST`
  budget are an open item (§11).

## 9. Decisions (D1–D6) and parameters {#decisions}

- **D1 — Keeper required** for coordination; the S3-only coordination mode is dropped (it had clock-skew and
  fence-steal data-loss modes). S3 remains the sole durable truth. **The epoch is NOT in Keeper and NOT
  process-cached:** it lives only in S3 as one **`gc/epoch_state {open_epoch, folded_through, leader_fence}`**,
  read **fresh once per build** (one `GET`, not per file). A TTL cache is rejected — it could hand out an
  *already-folded* epoch (`< folded_through`), a real loss path; a fresh read always pins to the current
  `open_epoch`, which is never folded. There is **no seal** (`gc/sealed`), no late reappend, and no cross-epoch
  dedup: a writer logs to its single pin epoch and the GC never folds an epoch `≥ safe_epoch`, so a live pin's
  epoch is inherently safe to append to. Keeper holds only the writer's **pin** (`writers/<S>`, the quiescence
  arbiter, frozen until commit/abort) and leader election.
- **D2 — generations decoupled** from epochs; the durable barrier is the **single `gc/retired` manifest** (D6),
  **not** a separate per-hash `floors` namespace. `floor_gen(H) := 1 + max retired gen for H` is *derived* from
  `gc/retired` (cached via the optional rebuildable FloorIndex): reuse iff `g ≥ floor_gen(H)`, else resurrect to
  `floor_gen(H)`. Because a retired entry is kept until its object is deleted, the manifest is the durable reuse
  authority that outlives any recent window — and the retire round `r` is the **delete-grace** reference: a
  retired `(H,g)` is physically deleted only once `safe_epoch > r` (+ a final recheck), because a writer that
  passed its reuse-check before the entry was written may still be committing a reference. No `g ≥ O_W` rule;
  long-lived dedup preserved (a never-retired hash has no entry → floor 0). The earlier "separate durable
  `floors/<H>`" framing was wrong — it re-introduced per-object tombstones; the barrier is the one consolidated
  manifest.
- **D3 — flat refs** per part for the table active set (no giant table tree; revisit a trie only if atomic
  table snapshots become required).
- **D4 — deferred decrement cascade** on tree delete (children reclaimed over later rounds; idempotent). The
  child-edge `-` deltas are read **from the tree object itself** and written durably *before* the object is
  deleted — so a crash before delete just re-reads the still-present tree (crash-safe); the `child_edges` carried
  in the `gc/retired` entry are an optimization/retry aid, **not** the authority.
- **D5 — trust `checksums.txt`** for part-trees: blob hash = `cityHash128` so copy/fetch/ATTACH build the
  part-tree and dedup from `checksums.txt` with no re-read/re-hash; fail-safe re-hash on mismatch.
- **D6 — orphan / drift reclamation = the periodic reconcile, authoritative-rebuild (write-ahead intents
  *rejected*).** The `O(delta)` fold cannot fix (a) never-referenced debris (no edge) or (b) stale positives (a
  `+` outliving a failed commit/drop). We considered a write-ahead per-object record (S3-log pin, then a Keeper
  `H:g → epoch` intent) to enumerate debris without a full `LIST` — and **rejected it**: for a *fresh* part the
  blob hashes are **not known up front** (`checksums.txt` is computed *as* the part is built), so the record
  degrades to `O(files)` hot-path writes — the data-proportional traffic this design exists to remove; and it
  cannot cancel a stale positive anyway. Instead the periodic **reconcile** (§4.5) **authoritatively rebuilds**
  in-degree from the real `refs/` reachability + the physical `LIST`, with a **high-watermark over the quiesced
  prefix** (`snap/<Q>` authoritative-through-`Q`, `Q < safe_epoch`; discard logs ≤ `Q`). This recomputes the true
  in-degree over the quiesced prefix using two explicit timestamps — count a root iff its ref's `commit_epoch ≤
  Q`, rebuild an object iff its `birth_epoch ≤ Q`. A *dead* writer's debris and stale positives end at in-degree
  0; a *live* writer's tentative edge (epoch `≥ safe_epoch > Q`) and its edge-less fresh upload
  (`birth_epoch > Q`) are both left untouched; the *same quiescence + in-degree + fence gate* reclaims what it
  finds. **Quiescence, not a wall clock, is the safety**; there is **no age-filter** — the quiesced prefix
  already protects in-flight objects (a filter would be redundant *and* unsafe). Decision: **no hot-path orphan
  tracking; reconcile is the authoritative truth-maker over the quiesced prefix; `commit_epoch`/`birth_epoch`
  make that prefix computable.** Accepted cost: debris / stale positives linger until the next reconcile, an
  `O(objects)`-but-`O(1)`-memory pass (§11).
- **Hashes:** `cityHash128` (blob), `SipHash-128` (canonical tree entries).
- **Parameters (tuning, conservative defaults):** epoch period ≈ 10 min; `T_session` ≈ 60 s; heartbeat ≈ 15 s;
  limbo = `Q+2`; reconcile cadence ≈ days. (One fresh `GET gc/epoch_state` per build — no epoch cache, no
  age-filter.)

## 10. Verification scope {#verification}

The TLA+ model (`docs/superpowers/models/`, extended from `CaGcCore.tla`) **must** cover, under adversarial
interleaving + the failure set (session-expiry gap, split-brain, total Keeper wipe): the single-node core
(already PASS) **plus the currently-untested** (a) **multi-child commit atomicity** — a tree making a *set* of
nodes reachable at once; (b) the **deferred decrement cascade** (D4); (c) the **decoupled reuse rule** (D2,
re-verifying CE-2 under `floor`-resurrection rather than the old `e ≥ O_W` encoding); (d) the **reconcile
quiesced-prefix rebuild** (§4.5): a `Reconcile` action that recomputes in-degree from `refs/`-reachability over
`commit_epoch ≤ Q` roots and `birth_epoch ≤ Q` objects — debris and stale positives end at in-degree 0, are
retired by the next fold, and deleted; an object with `birth_epoch > Q` is excluded. Properties: an
uploaded-but-uncommitted orphan **and** a node with a stale root `+` are eventually reclaimed (no leak); a
committed / dedup-reused / reachable object is **never** reclaimed even if reconcile ran (no loss); (e) the
**durable floor + retire-grace** (#3): a writer reuses `(H,0)` (floor-recheck sees `floor_gen=0`); the GC then
raises `floor_gen=1` + `retire_epoch`; `(H,0)` must **not** be deleted until `safe_epoch > retire_epoch` + final
recheck — model the floor-recheck-vs-floor-raise race and confirm no loss. Plus: a later writer cannot reuse a
deleted `(H,g)` (sees `g < floor_gen(H)`, resurrects) — no ABA when reclaim lags; (f) the **single-pin +
commit/abort-before-advance** headline (P1-1): a LIVE writer with a logged-but-unpublished root edge (and an
edge-less fresh upload), while `Reconcile` runs — neither must be reclaimed before commit/abort, because the
frozen pin keeps `safe_epoch ≤` build epoch (so fold/reconcile skip it) and `birth_epoch > Q` excludes the
upload. Invariants: `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_ABA`, and (temporal, if feasible) no-leak-forever.

**Out of scope of the model (assumed / handled elsewhere):** the fold/snap data-plane internals (LIST
pagination, on-disk merge — implementation, not protocol); the reconcile's **physical `LIST` + tree-walk
mechanics** (the model abstracts it as `Reconcile` recomputing in-degree from `Reachable(refs)` over the
quiesced prefix, gated by modeled `commit_epoch`/`birth_epoch`); multipart-upload invisibility + S3 lifecycle
abort (infra); the reader path; the exact canonical serialization bytes and the tree-entry `size`/`attrs`
(non-safety metadata). The model uses a **single S3 `epoch_state`** read freshly (no cache), a **single pin
epoch** per writer (writer logs to it, no append-epoch, no seal/reappend), the **idempotent edge log** (op_id
within the pin epoch), and **`birth_epoch`/`commit_epoch`** for the reconcile prefix. The S3-only coordination
mode is dropped (D1), so not modeled.

## 11. Open items and operational concerns {#open-items}

These do not change the protocol's safety argument but must be addressed before/within implementation; raised by
design review.

- **Observability.** Expose `system.*` so an operator can answer "I dropped a table, why didn't S3 shrink?":
  `open_epoch`, `folded_through`, `safe_epoch`, the oldest pinning writer (and its pin epoch), gc/retired-set
  size, reclaim backlog, last reconcile time, per-pool reclaimable-bytes estimate. Reclamation is intentionally
  lazy (limbo + quiescence), so this visibility is required, not optional.
- **Reconcile at scale.** The prefix-rebuild reconcile (§4.5) is `O(all objects)` but `O(1)`-memory (streaming
  `LIST ⋈ refs/-reachability` spill through the merge-sort — never materialize a reachability set). Still to
  specify: the **schedule** (e.g. every N days), the **per-pass `LIST` budget / rate limit** (at 10¹¹ objects a
  full `LIST` is ~10⁸ paginated calls — money and wall-clock), **how `birth_epoch` is read cheaply at scale**
  (system metadata vs a per-object `HEAD`), and **per-shard checkpointing** (an interrupted pass leaves un-rebuilt
  shards on their previous authoritative `snap/<Q'>` — always safe).
- **Generation compaction.** A node resurrected to `g>0` pays `GET`-`404`-then-`LIST` on every read forever; there
  is no path back to `g=0`. Decide whether reconcile compacts live nodes back to `g=0` (rewriting the referencing
  trees) or whether high-`g` nodes are tolerated as a rare read-cost item — **and in either case expose a
  `count of live nodes with g>0` metric** so "rare" can be observed before it stops being rare.
- **Stuck / flapping writer.** A single live-but-stalled writer pins `safe_epoch` and stalls pool-wide
  reclamation until its lease expires (liveness only, by design); a writer flapping near the session deadline can
  repeatedly pin/release. Surface "oldest pinning writer / reclamation lag" as an alertable metric.
- **`checksums.txt` coverage (D5).** The set of part files that become blobs must exactly match
  `IMergeTreeDataPart::checksums`. Some files are **not** in `checksums.txt` (e.g. `checksums.txt` itself,
  version-dependent metadata, the `.meta` sidecars) — those are table-level verbatim `files/` or `refs/*.meta`,
  not content-addressed blobs. The attach-time guard (D5) must verify the blob set round-trips **in both
  directions** — a *missing* file → `INV-NO-DANGLE` (loud, read fails); an *extra* file (a checksum that is also
  stored verbatim) → silent double-storage — falling back to re-hash on any mismatch.
- **Edge log + idempotence.** A retry within a writer's pin epoch reuses `op_id = hash(build_id, edge, sign)`,
  so add/remove are idempotent without any cross-epoch dedup (there is no cross-epoch reappend). Confirm the
  on-disk coalesced log + merge-sort fold treat the same `(parent,child)` edge once (add/remove last-writer-wins
  per key), not as a signed counter. `gc/epoch_state` is a single object updated under `leader_fence`.
- **`birth_epoch` / `commit_epoch` plumbing.** Set `birth_epoch` on every blob/tree object (immutable, retained
  on `createIfAbsent`) and extend `RefPayload` with `commit_epoch`. Decide where `birth_epoch` lives so reconcile
  can read it during the scan without a `HEAD` per object if possible. These two timestamps are what make the
  quiesced-prefix reconcile computable (count refs with `commit_epoch ≤ Q`, rebuild objects with
  `birth_epoch ≤ Q`); without them reconcile cannot tell a live writer's fresh upload from old debris.
- **FloorIndex (reuse-lookup cache).** The reuse-barrier is `gc/retired`; for an O(1) hot-path check, materialize
  an optional **FloorIndex** ("max retired gen per hash") as a rebuildable cache over `gc/retired` — it is a
  cache, not a second authority (rebuild from `gc/retired`; refresh / fail-closed before commit if stale). Confirm
  the lookup stays off the critical latency path (only consulted on reuse/resurrect), and that `gc/retired` stays
  bounded by the pending-reclaim backlog (entries dropped after their object is deleted).
- **Part-load read path.** `MergeTreeData::loadDataParts` / attach issue many `exists`/`getMetadata`/`list`
  calls; on the CA disk each becomes a tree walk. Verify a server start loading thousands of parts hits the `g=0`
  `GET` fast path and does not trip the cold `404→LIST` en masse.

---

## Appendix A. Formal model (TLA+-convertible) {#appendix}

Semi-formal; maps to TLA+ `CONSTANTS` / `VARIABLES` / `Init` / `Next` / `INVARIANT`. The existing
`CaGcCore.tla` is the single-node instance; this appendix is the target (multi-child) shape.

### A.1 Constants {#a-constants}
```
Hashes        finite set of content identities (blobs and trees), e.g. {h1,h2,t1}
Writers       finite set of writer identities, e.g. {w1,w2}
Leaders       finite set of GC-leader identities, e.g. {L1,L2}
MaxEpoch      bound on epochs (e.g. 2)
MaxGen        bound on per-node generations (e.g. 1)   -- D2 decoupled; usually 0
Children      Hashes -> SUBSET Hashes                   -- the (static) tree structure: a tree's child set
                                                        -- (no RetentionEps: A′ dropped the age-filter; reconcile is purely quiesced-prefix)
```

### A.2 Variables (state) {#a-vars}
```
\* ---- S3 (durable) ----
node       \in [Hashes \X Gen -> {Absent, Present, Deleted}]      \* object existence per (hash,gen)
birthEpoch \in [(Hashes \X Gen) -> Epoch \cup {None}]             \* immutable object metadata; set on create, retained on createIfAbsent
refs       \subseteq (RefName \X Hashes \X Gen \X Epoch)           \* live root edges; 4th field = commit_epoch (pin epoch of the root +)
edgeLog    \in [Epoch -> SUBSET (Edge \X {Add, Rem})]             \* gc/log: per-epoch ADD/REM of edges keyed by (parent,child); IDEMPOTENT (op_id)
snap       \in [(Hashes \X Gen) -> Int]                            \* folded in-degree (distinct present edges) through folded_through; rebuilt ≤Q
retired    \subseteq (Hashes \X Gen \X Epoch)                      \* gc/retired: (hash, gen, retire_round) — THE single durable barrier
                                                                  \* (reuse barrier + delete-grace ref); entry dropped after its object is deleted
\* floor_gen(H) == 1 + Max({g : <<H,g,_>> \in retired}) (0 if none) — DERIVED, not a separate variable (FloorIndex is a cache)
epochState \in [open_epoch: Epoch, folded_through: Epoch \cup {None}, leader_fence: Nat]  \* gc/epoch_state — ONE object; SOLE epoch authority (NOT in Keeper)
\* ---- Keeper (ephemeral) ----
leaderSeq  \in [Leaders -> Nat \cup {None}]                        \* leader/lock-<seq>; lowest live = leader; seq = fence
pinEpoch   \in [Writers -> Epoch \cup {None}]                      \* writers/<S> = the PIN; writer logs ALL deltas here, kept until commit/abort
\* ---- per-actor local ----
wConn      \in [Writers -> {Connected, Disconnected}]              \* writer's belief
wSess      \in [Writers -> {SessAlive, SessExpired}]               \* server-side session truth (may differ from wConn)
wDecided   \in [Writers -> SUBSET (Hashes \X Gen)]                 \* deps decided this build
wTentEdges \in [Writers -> SUBSET Edge]                            \* this writer's flushed-but-uncommitted edges (incl. root)
wPending   \in [Writers -> BOOLEAN]                                \* an uncommitted build is in flight (FREEZES pinEpoch advance, P1-1)
ldrConn    \in [Leaders -> {Connected, Disconnected}]
```
There is **no `appendEpoch` and no seal** — a writer logs every delta into its single `pinEpoch[w]` and the GC
never folds an epoch `≥ safeEpoch`, so the pin epoch is always safe to append to. **Edge-set fold:** an edge is
*present at horizon h* iff its highest-epoch op (≤ h) is `Add` (last-op-wins per `(parent,child)`; re-`Add`
idempotent, re-`Rem` a no-op). `inDegree(n,h)` = count of DISTINCT present edges `(_,n)`; `snap` is this folded
through `folded_through`. A fold CANDIDATE is `node[n]=Present ∧ inDegree(n)=0`.
`safeEpoch == Min({pinEpoch[w] : w live}) (or open_epoch if none)`; watermark `Q` with `folded_through < Q <
safeEpoch ∧ Q ≤ open_epoch−1`. **Reconcile rebuilds `snap` from `Reachable(refs)` over the prefix `≤ Q`**, using
`commit_epoch` (count a ref iff its `commit_epoch ≤ Q`) and `birthEpoch` (rebuild an object iff `birthEpoch ≤
Q`, exclude `> Q`). Reuse authority is the durable `retired` set: `reusable(H,g) == g ≥ floor_gen(H)` (derived).
While `wPending[w]`, `pinEpoch[w]` is FROZEN; `W_AdvancePin` is disabled until commit/abort.

### A.3 Init {#a-init}
All nodes `Absent`; `birthEpoch = None`; `refs = {}`; `edgeLog` empty; `snap = 0`;
`retired = {}`;
`epochState = [open_epoch ↦ 0, folded_through ↦ None, leader_fence ↦ 0]`; no leader; all `pinEpoch = None`;
all `wPending = FALSE`; writers `Connected ∧ SessAlive` with empty decided/tentative sets.

### A.4 Actions (Next = disjunction) {#a-actions}
Each action: **guard ⇒ effect.** (`Edge` = `(parent, child)` where parent is a ref or a tree node.)

- **W_RegisterLease(w):** guard `pinEpoch[w]=None ∧ wConn[w]=Connected ∧ wSess[w]=SessAlive`.
  effect `pinEpoch[w] := epochState.open_epoch` (read fresh — never below `folded_through`).
- **W_CreateOrReuseChild(w, c):** guard writer may-act (`wConn=Connected ∧ wSess=SessAlive`). let `fg :=
  floor_gen(c)` (derived from `retired`). effect: `wPending[w] := TRUE`; let `g` be the present gen of `c`; if `node[c,g]=Present
  ∧ g ≥ fg` → reuse (add `(c,g)` to `wDecided[w]`); else `node[c, fg] := Present`, `birthEpoch[(c,fg)] :=
  pinEpoch[w]`, add `(c, fg)` to `wDecided[w]` (resurrect to the durable floor_gen, D2/#3).
- **W_FlushEdge(w, edge):** guard `edge`'s child `∈ wDecided[w]`. effect add `(edge, Add)` to
  `edgeLog[pinEpoch[w]]` (always the pin epoch — **no seal check, no re-target**; idempotent by edge key) and to
  `wTentEdges[w]`.
- **W_PublishRef(w, name, (T,g)):** guard may-act ∧ `(T,g)`'s edges `∈ wTentEdges[w]` ∧ **every decided child
  `(c,g_c)` still has `g_c ≥ floor_gen(c)`** (re-check vs `retired`, P1-3 — a moved floor disables commit and
  forces `W_Abort`+rebuild). effect add `(name, T, g, pinEpoch[w])` to `refs` (record `commit_epoch = pinEpoch[w]`);
  `wPending[w] := FALSE`; clear `wDecided[w]`, `wTentEdges[w]`.   (durable OUTCOME — `W_AdvancePin` now enabled)
- **W_Abort(w):** guard `wPending[w]=TRUE`. effect (explicit abort, P1-1): for every edge ∈ `wTentEdges[w]`
  (incl. the tentative root) add `(edge, Rem)` to `edgeLog[pinEpoch[w]]`; `wPending[w] := FALSE`; clear
  `wDecided`/`wTentEdges`. (A crash instead just expires; tentative edges become a stale positive cleaned by reconcile.)
- **W_AdvancePin(w):** guard `wConn=Connected ∧ wSess=SessAlive ∧ pinEpoch[w] < epochState.open_epoch ∧
  ¬wPending[w]` (**commit/abort-before-advance, P1-1**: advance the pin only with no uncommitted build).
  effect `pinEpoch[w] := epochState.open_epoch` (re-read fresh).
- **W_Drop(w, name):** guard may-act. effect remove the ref from `refs`, then add `((ref,(T,g)), Rem)` to
  `edgeLog[pinEpoch[w]]`.
- **GC_Close(L):** guard `IsLeader(L)` (lowest live `leaderSeq`) ∧ `ldrConn[L]=Connected`. effect fenced PUT
  `epochState := [open_epoch ↦ open_epoch+1, folded_through ↦ folded_through, leader_fence ↦ leaderSeq[L]]`.
  (No seal: pinned writers may still append to their epoch; GC never folds `≥ safeEpoch`.)
- **GC_Fold(L, Q):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ folded_through < Q ∧ Q < safeEpoch ∧
  Q ≤ open_epoch−1`. effect fold edge-set ops of `edgeLog[(folded_through, Q]]` onto `snap`; set
  `epochState.folded_through := Q` (fenced); logs `≤ Q` may be discarded. (The in-degree-0 nodes are the input to
  `GC_Retire`; materializing them is an optional disposable work-list, not state.)
- **GC_Retire(L, n=(H,g)):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ node[n]=Present ∧ inDegree(n)=0`. effect
  **durably add** `<<H, g, epochState.open_epoch>>` to `retired` (the single barrier — blocks NEW reuse of `(H,g)`
  via `floor_gen`, and records the retire round `r = open_epoch` as the delete-grace ref). NOT a delete — the
  object may still be reused/reachable until the grace + final recheck.
- **GC_Delete(L, n=(H,g)):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ <<H,g,r>> \in retired ∧
  safeEpoch > r  (GRACE) ∧ inDegree(n)=0 (final recheck on the CURRENT fold) ∧ n ∉ Reachable(refs)`. effect: if
  `n` is a tree, FIRST for each child edge (read from the STILL-PRESENT tree object) add `(edge, Rem)` to
  `edgeLog[open_epoch]` (deferred cascade, D4), THEN `node[n] := Deleted`; the `retired` entry MAY then be dropped
  (its barrier is moot post-delete). (No separate floors/candidates state, no `+2` limbo — `safeEpoch > r` +
  recheck is the whole barrier.)
- **Reconcile(L):** guard `IsLeader(L) ∧ ldrConn[L]=Connected`. effect (prefix rebuild): pick `Q` with
  `folded_through < Q < safeEpoch ∧ Q ≤ open_epoch−1`; rebuild `snap` for every present `n` as the count of
  distinct edges `(_,n)` from `Reachable({r ∈ refs : r.commit_epoch ≤ Q})`, **counted only for `n` with
  `birthEpoch[n] ≤ Q`** (objects with `birthEpoch > Q` are EXCLUDED — a live writer's fresh upload is not misread
  as debris); drop `edgeLog[≤Q]`; `folded_through := Q`. The resulting in-degree-0 nodes feed the SAME `GC_Retire`
  → `GC_Delete` path (add to `retired`, then grace + recheck). Reconcile never deletes. `Reachable` is the DAG
  closure from the live roots — the durable authority, NOT `snap`.
- **Failures:** `Sess_Expire(w)` (`wSess[w]:=SessExpired ∧ pinEpoch[w]:=None ∧ wPending[w]:=FALSE` — build
  abandoned by death; tentative edges remain a stale positive cleaned by reconcile, fresh uploads stay protected
  by `birthEpoch` until that epoch quiesces); `Disconnect(w)`/`Reconnect(w)`; `Ldr_Disconnect(L)`/`Ldr_Steal(L)`;
  `Keeper_Wipe` (all `pinEpoch:=None ∧ wPending:=FALSE`, all `leaderSeq:=None`, writers→read-only;
  **`edgeLog`/`retired`/`snap`/`epochState`/`birthEpoch`/`refs` are S3, NOT cleared**); `Recover` (purge
  `leader/`+`writers/` only — no epoch znode; elect, `epochState.open_epoch += 1`, re-register).

### A.5 Invariants {#a-inv}
```
INV_NO_LOSS    == \A (name,T,g) \in refs : node[T,g] # Deleted /\ \A c \in reachable(T,g): node[c] # Deleted
INV_NO_DANGLE  == \A (name,T,g) \in refs : node[T,g] = Present
INV_NO_ABA     == \A n : (n was Deleted) => [](node[n] # Present)        \* via everDeleted history var
INV_TYPE_OK    == ... \* domains
```
(`reachable(T,g)` = the DAG closure through `Children`.)

### A.6 Liveness (temporal, optional) {#a-live}
```
NoLeakForever  == \A n : (eventually-always unreachable(n)) ~> (eventually node[n] = Deleted)
```

### A.7 Suggested finite bounds for TLC {#a-bounds}
`Writers = {w1,w2}`, `Leaders = {L1,L2}`, `MaxEpoch = 2`, `MaxGen = 2` (so a floor can move 0→1→2), a tree with
**2 children** (multi-child). `StateConstraint` caps `|refs| ≤ 2`, per-writer `wDecided`/`wTentEdges` **≤ 3**
(a commit involves a tree + its 2 children — the old ≤2 made multi-child commit unreachable, #5b),
`epochState.open_epoch ≤ MaxEpoch+2`. Bounded model checking — strong evidence within bounds, not a proof.

**Scenarios that MUST be checked:**
- **No-leak (debris):** `W_CreateOrReuseChild` then `Sess_Expire` *before* `W_PublishRef` (uploaded, never
  committed). Its `birthEpoch` epoch quiesces after death → `Reconcile` rebuilds it at in-degree 0 → `GC_Retire`
  → `GC_Delete` removes it.
- **No-leak (stale positive):** the `(ref,(T,g))` `+` is logged but the writer dies before `W_PublishRef` (no
  ref) → fold over-counts `T` → after the epoch quiesces, `Reconcile` recomputes `snap[T]=0` (T ∉ Reachable from
  refs with `commit_epoch ≤ Q`) → `T` reclaimed.
- **No-loss (P1-1 headline):** a LIVE writer logs `(ref,(T,g))`, has NOT published the ref, and `Reconcile` runs
  (writer possibly on an old pin from a lagging read). `T` must **never** be `Deleted` before commit/abort: the
  frozen `pinEpoch` keeps `safeEpoch ≤` build epoch, so `Q < safeEpoch` leaves `T`'s epoch un-rebuilt and the
  delete gate fails. Drive every interleaving of `W_FlushEdge`/`W_AdvancePin`/`GC_Fold`/`Reconcile`/`GC_Retire`/
  `GC_Delete` against an uncommitted live build.
- **No-loss (fresh edge-less upload, the birth_epoch case):** `W_CreateOrReuseChild` creates a node with
  `birthEpoch = pinEpoch` but the writer logs **no edge yet** and stays live. `Reconcile` must EXCLUDE it
  (`birthEpoch > Q`) and never retire/delete it.
- **No-loss (reachable):** a reachable node (committed / dedup-reused) is never `Deleted`, even after `Reconcile`.
- **Retire-grace race (the new headline):** a live writer reuses `(H,0)` (floor-recheck saw `floor_gen=0`); GC
  `GC_Retire`s `(H,0)` (raises `floor_gen=1`, `retire_epoch`). `GC_Delete` must be blocked until
  `safe_epoch > retire_epoch` ∧ final recheck — so the still-building writer (pin ≤ retire_epoch) is waited out;
  if it commits, the recheck sees reachability and spares `(H,0)`; if it dies, `(H,0)` is deleted. No loss.
- **No-ABA-when-reclaim-lags (floor):** retire `(H,0)` (raises `floor_gen=1`), delay reclaim past the recent
  window, then a new writer wanting `H` must resurrect to `floor_gen(H)` (=1, derived from `retired`), never reuse `(H,0)`.
- **Floor-move-before-commit (P1-3):** a reused child's `floor` moves after its parent tree was flushed →
  `W_PublishRef` disabled (re-check fails) → `W_Abort` + rebuild → no ref ever names a below-floor child.
