---
description: 'Authoritative design specification for the content-addressed (CA) MergeTree storage backend — a Merkle DAG of immutable folders reclaimed by a Keeper-coordinated, S3-durable Epoch-Based-Reclamation GC. Narrative design plus a TLA+-convertible formal appendix (state, guarded actions, invariants).'
sidebar_label: 'CA Merkle store design spec'
sidebar_position: 1
slug: /superpowers/specs/ca-merkle-store-design
title: 'Content-Addressed MergeTree Storage — Design Specification'
doc_type: 'guide'
---

# Content-Addressed MergeTree Storage — Design Specification {#ca-merkle-store-design}

**Status:** authoritative, single-source. Supersedes the exploration docs in `docs/superpowers/reports/`
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
- **Ref** — the only mutable object: a named pointer `name → (node_hash, gen, header)`. Refs are the GC roots
  and the commit points (written last, removed first).

**Node** = a blob or a tree. **Edge** = a reference from a ref to a node (`ref → (T,g)`) or from a tree to a
child (`(T,g) → (child,g)`). The store is therefore a **Merkle DAG, acyclic by construction** (a parent's hash
needs its children's hashes to exist first), so **per-node reference counting is complete** — `in-degree == 0`
means genuinely unreachable; no cycle collector is ever needed.

**Generations (D2).** A node key carries a generation `g`: a **small per-node resurrection counter**, `0` until
the GC condemns generation `g` and a writer needs that content again and recreates it above the **durable
per-hash floor** `floors/<H>` (= 1 + max-condemned-gen; #3). `g` is **decoupled from the epoch**; it exists only
so a GC delete of `(N,g)` and a concurrent re-create can never collide on a key (the re-create goes to a gen
`≥ floor`, a different key — G2/ABA). The floor is durable so this holds even when a reclaim is delayed far past
the recent-condemned window.

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
  blobs/<H[:2]>/<H>/<g>                    immutable FILE bytes — DAG leaf node (H,g);  g = per-node resurrection gen (0 common)
  trees/<T[:2]>/<T>/<g>                     immutable FOLDER — canonical entries [(name, kind, child_hash, child_gen, size, attrs)] — node (T,g)
  roots/<server_id>/                        mutable GC ROOTS, mirroring the normal ClickHouse on-disk tree
      store/<uuid[:3]>/<uuid>/
          refs/<part_name>                  LIVE REF -> RefPayload{ T, g, header } — GC ROOT, commit point (written last, removed first)
          refs/<part_name>.meta             mutable per-part sidecar files (verbatim)
          refs/detached/<name>              detached ref (root, not in the active set)
          files/<tail>                      table-level verbatim files (format_version.txt, …)
      shadow/<backup_id>/store/<uuid[:3]>/<uuid>/refs/<part_name>   FROZEN ref — GC root, table-lifetime-independent
                                              (same table-level layout as store/ above — mirrors the usual shadow path)
  gc/epoch_state                             ONE durable object { open_epoch, sealed_through } — the SOLE epoch authority,
                                              updated ATOMICALLY by the fenced leader at R0 (close = {open_epoch:E+1, sealed_through:E}).
                                              No separate seal namespace, no bump/seal ordering race. Writers read it from S3
                                              (process-cached, §4.1): open_epoch = where to append; sealed_through = what is foldable.
  gc/log/<epoch>/<shard>                     EDGE-SET log: records ADD/REMOVE of edges keyed by (parent_node, child_node) — root
                                              edges keyed (ref_name, node). IDEMPOTENT: a re-added edge does not increment in-degree
                                              twice; a re-removed edge is a no-op. (No event_id dedup needed — the edge key IS the
                                              identity.) Coalesced (group-commit), hash-prefix sharded.
  gc/snap/<Q>/<shard>                        folded IN-DEGREE = count of DISTINCT present edges per node, sorted by node key, sharded.
                                              snap/<Q> is AUTHORITATIVE THROUGH the quiesced prefix Q (high-watermark, Q < safe_epoch):
                                              logs ≤ Q are discarded once reconcile rebuilds it (§4.5)
  gc/condemned/<epoch>                      FULL reclaim record/round (NOT just a node set): per condemned node
                                              { (hash,gen), kind ∈ {blob,tree}, child-edges (for trees), fold-epoch } — so an
                                              interrupted cascade is completed by a successor from this durable record (§4.2 R4)
  floors/<H>                                 DURABLE per-hash generation floor = 1 + max gen ever condemned for H (SPARSE — only
                                              condemned hashes have one). Reuse authority that outlives the condemned-read window (§4.1, D2)
  _pool_meta                                pool identity / format version
```
Notes: all mutable roots live under `roots/<server_id>/` and **mirror the real ClickHouse path** (`store/<uuid>`
for live tables, `shadow/<backup_id>/store/<uuid>` for frozen backups) so the metadata layer maps onto it
directly. There is **no per-object tombstone** — condemnation is the per-epoch `gc/condemned/<e>` record. There is
**no `active` hint**: a node's present generation is resolved by a `GET <hash>/g=0` first (the **>90% case** —
`g` stays `0`), and only on `404` a `LIST <hash>/` (a `LIST` costs ≈10× a `GET`, so this is a **cold** path, not
the hot path). Dropping the `active` hint removes the whole class of races around keeping it in sync, paying only
that rare cold `LIST`. Condemnation gates *reuse/attachment*, never *reads*. Root edges (`ref→T`) ARE logged (so
the routine fold needs no `LIST` of `refs/`); because the log is an **idempotent edge-set** and the §4.1
orderings bias a crash to **over-count (leak), never under-count (loss)**, reconcile (§4.5) authoritatively
cancels any stale positive over the quiesced prefix.

### 3.2 Keeper (ephemeral coordination only, `O(active writers)`) {#layout-keeper}
```
/clickhouse/ca/<pool>/
  leader/lock-<seq>     EPHEMERAL-SEQUENTIAL; lowest live seq = GC leader; <seq> IS the monotone fence token
  writers/<session_id>  EPHEMERAL; data = that writer's PIN epoch (the quiescence pin) — FROZEN while the writer
                        has an uncommitted build (wPending); distinct from the writer's local log-APPEND epoch (§4.1)
```
That is all Keeper holds — leader election (with the fence token) and one tiny ephemeral node per live writer.
The `writers/<S>` value is purely the **quiescence pin**: it never moves while a build is uncommitted, so a
build's tentative nodes stay protected by `safe_epoch`. Where the writer *appends* its log deltas (the append
epoch) is **local** state that may refresh/reappend freely as epochs seal — it does NOT touch Keeper.
**The epoch is NOT in Keeper** (v1 decision): it lives only in S3 `gc/epoch_state`; writers read it from S3 with
a short process-memory TTL (§4.1). A stale local read can only *lag* = safe; the `sealed_through` field is the
authoritative signal that forces a re-target when a late append would hit a closed epoch. Keeping the epoch out
of Keeper removes the fragile "Keeper-never-ahead-of-S3" invariant. Nothing in Keeper is authoritative durable
state, so a Keeper wipe stays non-destructive (`INV-S3-COMPLETE`).

## 4. Protocols {#protocols}

### 4.1 Writer — commit part `name` {#proto-write}
1. **Lease.** Hold a live Keeper session `S`; read `gc/epoch_state` from S3 (**process-cached, short TTL** `≪`
   epoch period, e.g. ~30 s — ~1 S3 `GET` per TTL window per process, not per commit; a stale read only *lags* =
   safe). The **pin epoch** `P_W := open_epoch`; publish `writers/<S> = P_W` (the quiescence pin). The local
   **append epoch** `A_W := open_epoch` too. **Self-fence (D1 hinge):** a commit may proceed only while
   `Connected ∧ local_elapsed_since_renew < T_session − margin`; else the writer goes **read-only**.
   (`Disconnected`-detection alone is insufficient — TLC CE-4.)
2. **Floor / condemned cache.** For reuse-safety, the authority is the durable per-hash floor `floors/<H>` (D2,
   §3.1): cache it lazily; `condemned?(H,g) ⇔ g < floor(H)`. (The recent `gc/condemned/<e>` sets are an optional
   fast-path hint, but the floor — not the bounded recent window — is what guarantees safety when reclaim lags.)
3. **Build locally.** For each file `f`: `Bf := cityHash128(f)` (D5: from `checksums.txt` for part files);
   `createIfAbsent blobs/<Bf>/<g>`; upload once if absent. For each subdir: recurse → child tree. **Reuse** the
   present generation `(hash,g)` iff present ∧ `g ≥ floor(hash)`; else **resurrect**: `createIfAbsent` at
   `floor(hash)` (≥ `gen+1`). (No write-ahead orphan tracking on this path — see §9; crash debris is collected by
   the periodic reconcile, §4.5.)
4. **Tree.** `T := SipHash-128(canonical entries)`; `createIfAbsent trees/<T>/<g>`. (`part_id ≡ T`.)
5. **Edge `+`.** Add edges to `gc/log/<A_W>/<shard>` as an **idempotent edge-set** — root `(ref_name, (T,g))`
   and each `((T,g), child(h,g))` — keyed by `(parent,child)`, so a re-added edge never increments in-degree
   twice. **Append-epoch refresh (P1-2, normative — closes the check-then-append TOCTOU):** make the add durable,
   then re-read `gc/epoch_state`; if `A_W ≤ sealed_through` (the leader sealed it), set `A_W := open_epoch` and
   **re-add the same edges** there (idempotent) — loop until the edges are durable in an epoch still open at
   add-time. **Only `A_W` moves here — the pin `P_W`/`writers/<S>` does NOT** (P1-1 split). An edge so confirmed
   is "flushed".
   **Floor re-check + ANCESTOR REBUILD (P1-3, normative).** Re-check `floor` for every reused child. If any reused
   `(h,g)` now has `g < floor(h)`, the child must resurrect to `floor(h)` — **but tree identity includes
   `child_gen` (§2), so the parent tree's hash changes, and so does every ancestor up to the ref.** Therefore:
   **abort the whole tentative chain above `h`** (remove its already-flushed edges), **rebuild** the chain
   `h → … → T` with the new gens, **flush** the complete new edge set, and re-check. A floor move invalidates the
   tentative DAG above the node — never patch a single child in place.
6. **Commit.** `setRef roots/<server_id>/store/.../refs/<name> = RefPayload{T,g,header}` — the commit point,
   **written last** (re-check the self-fence immediately before; re-confirm the floor re-check passed).
   **Pin-advance gate (P1-1, normative — flush-COMMIT/ABORT-then-advance).** The pin `P_W` (`writers/<S>`) advances
   **only after a durable OUTCOME** for this build: either (commit) `setRef` is durable, or (explicit abort) the
   removal of every tentative edge — including the root `(ref_name,(T,g))` — is durable. Advancing the pin on a
   durable `+` alone is **forbidden**: it would release quiescence on the build epoch while the tentative root
   edge is unmatched by a real ref, letting reconcile (which sees no ref) zero the node and GC reclaim it before
   `setRef`. A crash with neither outcome is covered by session expiry (the lease drops → the build epoch
   quiesces → reconcile cleans the tentative edges). (`A_W` may have moved ahead freely; only `P_W` is gated.)
7. **Drop.** `removeRef` **first**, then append the `ref→(T,g)` `-` delta. (`removeRef`-before-`-` biases a crash
   to over-count, never loss.)

A crash before step 6 leaves the part not-live; because **the pin `P_W` is frozen at the build epoch until the
durable outcome** (step 6 gate), the writer's quiescence protects the tentative node until it commits or its
lease drops.
Both crash windows around the root edge — `+`-logged-but-no-ref (5↛6) and ref-removed-but-no-`-` (step 7) — leave
a **stale positive** that makes the fold *over-count* (the node lingers = leak), **never under-count** (a live
node is never seen as in-degree 0). Such stale positives are cancelled by the periodic reconcile (§4.5), which
authoritatively rebuilds in-degree from the real `refs/` — but **only over the quiesced epoch prefix** (§4.5), so
it never touches a live writer's still-pinned epoch. Nodes uploaded but *never* `+`'d (a crash between step 3 and
step 5) carry no edge — they
are invisible to the `O(delta)` fold and are likewise collected by reconcile (which `LIST`s physical objects).
Over-count only in every case — the accepted cost is that such debris lingers until the next reconcile.

### 4.2 GC leader — one round (fenced single deleter) {#proto-gc}
Guard every step: I am the lowest-seq child of `leader/` (a **`sync`-ed** read) ∧ `Connected`; else **fail-close
(stop)**. A **new** leader re-folds + re-quiesces under its **own** fence before any delete.
```
R0 CLOSE    fenced ATOMIC update gc/epoch_state := { open_epoch: E+1, sealed_through: E } (one durable PUT — no
            bump/seal ordering race). A writer whose append epoch ≤ sealed_through re-targets to open_epoch and
            re-adds its still-needed edges (idempotent — §4.1 step 5). This does NOT move any writer's pin.
R1 FOLD     epochs ≤ sealed_through only: per shard, streaming merge-sort gc/log/<≤s>/<shard> ⋈ gc/snap/<…>/<shard>
            → updated snap; IN-DEGREE = count of DISTINCT present edges per node; emit in-degree-0 CANDIDATES.
R2 CONDEMN  write the FULL reclaim record to gc/condemned/<e_a> (fenced; e_a = folded epoch): for each candidate,
            { (hash,gen), kind, child-edges if a tree, e_a }. For each condemned (H,g): bump the durable floor
            floors/<H> := max(floor, g+1) (so future writers resurrect above it — D2/#3).
R3 QUIESCE  safe_epoch := min(pin epoch) over getChildren(writers/) [SYNC-ED read — required: a stale read could
            under-count live writers]; if none → open_epoch (also via a sync-ed membership read).
R4 RECLAIM  for each entry (N,e_a) in gc/condemned/<e_a> with
              E_cur ≥ e_a+2  ∧  safe_epoch > e_a  ∧  in-degree==0 (re-read from the CURRENT fold, not a frozen
                                                                   snapshot)  ∧  still-leader(fence):
                if N is a TREE, FIRST append the `-` child-edge deltas (child list read from THIS durable record —
                  crash-safe: a successor completes the cascade from the same record), THEN DELETE the node object
                (DEFERRED CASCADE — children whose in-degree reaches 0 are condemned in a LATER round, D4).
```
**Two gears, not four.** The real no-loss gate is **quiescence** (`safe_epoch > e_a`: every live writer has
observed past `e_a`, so none can newly reference the node). `E_cur ≥ e_a+2` (Crossbeam 3-epoch limbo) is only the
conservative *slack* that lets quiescence be sampled lazily across rounds rather than synchronously. The
**deferred cascade** (D4) is not a separate mechanism — a tree-delete's child decrements are just `-` edges
folded next round. The fence gates every DELETE. There is **no wall-clock guard on the reclaim path** —
quiescence is the whole safety; the reconcile age-filter (§4.5) is a performance heuristic, never a safety gate.

### 4.3 Reader {#proto-read}
`GET ref → (T,g)`; `resolveNode(x,g)`: `GET <x>/<g>`, on `404` `LIST <x>/` and read any present generation;
walk tree entries, recurse into child trees, `GET` child blobs (`404 → LIST` at each node). A present-but-
condemned node still reads correctly (condemnation blocks only new reuse/attach). If both the `GET` **and** the
`LIST` are empty, the node was reclaimed under a dropped ref: **re-resolve from the ref**; if the ref itself is
gone the part was dropped and the read fails cleanly (no live ref names it — this is not a dangle).

### 4.4 Recovery — total Keeper loss {#proto-recovery}
```
during:  every writer session drops → READ-ONLY (self-fence); the leader's election znode is gone → GC stops.
         No mutation that could dangle or lose data occurs while Keeper is down.  SAFE PAUSE.
restore (even empty Keeper):
         PURGE any backup-restored ghost znodes — leader/ and writers/ (the epoch is NOT in Keeper, so there is no
         ghost-epoch hazard to purge — a direct benefit of the v1 decision).
         Elect a leader;  read gc/epoch_state (S3);  fenced ATOMIC gc/epoch_state := {open_epoch:E+1,
         sealed_through:E}  (fence off pre-outage in-flight epochs);  writers reconnect, re-create writers/<S>,
         read epoch_state from S3, resume.
         If gc/snap integrity is in doubt → RECONCILE (§4.5): authoritatively rebuild gc/snap from refs/
         reachability + the physical object LIST (this also cancels any stale positive and discovers debris); the
         usual quiescence gate reclaims what it finds.  S3 ALONE SUFFICES.
```

### 4.5 Reconcile — authoritative rebuild (corrects drift + discovers debris) {#proto-reconcile}
The `O(delta)` fold is fast but only ever *adds and subtracts* logged deltas, so it cannot fix two things: (a)
**never-referenced debris** (objects a crashed build PUT before publishing any `+`, §4.1) — no edge, invisible to
the fold; and (b) **stale positives** — a `+` left by a crashed commit/drop (§4.1 steps 5–7), which makes the
fold *over-count* (a leak). A periodic background **reconcile** (rare — e.g. every few days) fixes both by being
the **authoritative rebuild** of the in-degree snapshot from the durable roots — *not* an additive marker pass
(a zero-weight marker could never cancel a stale positive). It is `O(1)`-memory via streaming sorted merges.
```
QP  QUIESCED-PREFIX WATERMARK (P1-1 refinement 2, normative).  Choose Q with  Q < safe_epoch  — i.e. EVERY live
    writer (including a lagging-O_W one, whose low published O_W *lowers* safe_epoch) has advanced strictly past Q.
    Reconcile rebuilds / discards logs ONLY for epochs ≤ Q. It MUST NOT use "current E" or "sealed epoch" as the
    watermark: a live writer's tentative `+` sits in its epoch ≥ safe_epoch > Q, so it is in [Q+1, E_cur] and is
    NEVER rebuilt away — its pin survives. (This is the fix for "reconcile erasing a live pre-commit pin".)
R0  WALK reachability from all roots (refs/, refs/detached, shadow/) — the durable, written-last AUTHORITY (NOT
    gc/snap, which may have drifted) — spilling each reachable (hash,gen) and the internal-edge structure to an
    external SORTED stream. Reads live trees (≪ blobs) once. Read refs/ no older than the LIST (R1).
R1  LIST blobs/ + trees/  (paginated, sorted by key = the sharded node-key order).
R2  STREAMING MERGE  LIST ⋈ reachability-stream, per shard → REBUILD gc/snap/<Q> with the TRUE in-degree
    CONTRIBUTED BY epochs ≤ Q of every physically-present node. OVERWRITES drift within the quiesced prefix: a
    stale-positive whose `+` was in a now-quiesced epoch recomputes to its real value; never-referenced debris
    appears at in-degree 0. Write gc/snap/<Q> AUTHORITATIVE-THROUGH-Q and DISCARD logs ≤ Q.
R3  The next normal GC round folds logs in (Q, E_cur] onto snap/<Q> (so a live writer's tentative `+` in [Q+1,..]
    still counts); any node at in-degree 0 is condemned and reclaimed under the UNCHANGED gate
    (safe_epoch > e_a ∧ in-degree==0 ∧ E_cur ≥ e_a+2 ∧ fence). Reconcile itself never deletes.
```
**Why authoritative rebuild, not additive markers.** Stale positives (crashed commit/drop) and torn snapshots
both require *recomputing* in-degree from the durable roots, which a zero-weight marker cannot do. Walking
`refs/` (the written-last authority) and counting real edges gives the true in-degree over the quiesced prefix;
the high-watermark then retires those superseded logs so the stale `+` cannot re-appear. One pass does debris
discovery, stale-positive cancellation, AND the torn-snap rebuild (§4.4, §7).

**Why only the quiesced prefix (`Q < safe_epoch`).** A live writer's tentative root `+` (logged in step 5,
not yet matched by a `setRef`) looks "stale" to a `refs/` walk — it is NOT, the ref is coming. Rebuilding its
epoch would zero the node and (since the writer may have a lagging `O_W`) let GC reclaim it before commit — the
exact P1-1 loss. Restricting the rebuild to `Q < safe_epoch` guarantees no live writer is still in a rebuilt
epoch, so only *dead* writers' tentative `+`s (whose epochs have quiesced) are cancelled. Combined with the
step-6 advance gate (O_W pinned until commit/abort durable), an epoch falls below `Q` only after its writers
have committed (→ reachable, kept) or died (→ orphan, zeroed).

**`O(1)`-memory.** The `LIST` stream and the reachability spill are both sorted by node key and merged in a
stream; no reachability *set* is materialized. The expensive part is reading every live tree once (bounded by
part-count `≪` blobs) and the full `LIST` (the open item in §11).

**Quiescence is the safety, not a clock.** Reconcile only makes nodes *candidates* (in-degree 0 in the rebuilt
snapshot); the unchanged gate decides deletion. A live writer holding a tentative build has its **pin `P_W`
frozen at the build epoch until a durable commit/abort outcome** (step 6 gate), pinning `safe_epoch` at-or-below
that epoch — so reclaim is blocked until it commits (→ in-degree > 0) or dies (→ true orphan). A writer racing to
reference a candidate after the rebuild either bumps its in-degree (durable `+` re-adds → spared) or sees the
floor moved and resurrects to `floor` (re-check, §4.1 step 5). No wall clock on the reclaim path.

**No age-filter** (A′ simplification). The earlier age-filter was redundant: the quiesced-prefix `Q < safe_epoch`
*already* leaves every in-flight object (epoch `≥ safe_epoch > Q`) un-rebuilt, so reconcile cannot disturb a
live build — quiescence does the age-filter's job, exactly and without a clock. (A skipped young entry under a
filter could also have had its only protection in a discarded `≤Q` log — so a filter would have been not just
redundant but unsafe.) An interrupted reconcile is always safe — it just rebuilds fewer shards through `Q` this
pass (an un-rebuilt shard stays on its previous authoritative `snap`).

## 5. Invariants and the safety argument {#invariants}

- **INV-NO-LOSS:** a node `(N,g)` is deleted only when unreferenced in the fold **and** `safe_epoch > e_a`
  **and** `E_cur ≥ e_a+2` **and** the leader still holds its fence. EBR quiescence (`safe_epoch > e_a` ⇒ every
  live writer has observed past `e_a`, so it has the condemned set and will not newly reference `(N,g)`),
  combined with the fold showing in-degree 0, makes "no future reference can appear" hold at the delete point.
- **INV-NO-DANGLE:** a published ref always resolves to present bytes (reads tolerate a stale generation via
  `LIST`).
- **INV-NO-ABA:** a node once deleted is never returned to present at the **same** key — a re-create uses a gen
  `≥ floor(H)` (D2/#3, the durable per-hash floor), a different key; only `create-if-absent` is needed.
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
- **Liveness (no-leak-forever):** every genuinely unreachable node is eventually condemned (in-degree 0 fold)
  and, after the limbo + quiescence, reclaimed; deferred cascades drain dead subtrees over successive rounds; a
  stuck writer cannot stall reclamation forever (its lease expires → it is dropped from `safe_epoch`).

## 6. The hinges (assumptions correctness rests on) {#hinges}

1. **Writer self-fence on the Keeper session** (D1): a consequential op (reuse-commit) requires
   `Connected ∧ session-alive-by-local-clock` — fail-stop on `Disconnected`, deadline strictly inside
   `T_session`. This is a session-timeout assumption (no inter-clock skew, because Keeper's session is the
   single arbiter). The leader likewise fail-closes on `Disconnected`, with a `sync`-ed lowest-seq fence
   re-check before every delete.
2. **Pin/append split + flush-COMMIT/ABORT-then-advance + edge-set log + quiesced-prefix reconcile** (the
   P1-1/P1-2 hinge): (i) The writer's **pin** `P_W` (`writers/<S>`, the quiescence arbiter) is **frozen** until a
   **durable outcome** — `setRef` (commit) *or* a durable removal of every tentative edge (explicit abort); never
   moved on a durable `+` alone (§4.1 step 6). So while a part is in flight, quiescence protects its tentative
   nodes. The separate **append epoch** `A_W` (local, not in Keeper) may refresh freely. (ii) The leader closes
   with one **atomic** `gc/epoch_state {open_epoch, sealed_through}` update (no bump/seal race); the fold processes
   only `≤ sealed_through`; a writer re-reads `epoch_state` after a durable add and **re-adds** to `open_epoch` if
   its append epoch was sealed. The log is an **idempotent edge-set** keyed by `(parent,child)`, so a re-add never
   double-counts and the check-then-append race is closed without `event_id` bookkeeping. The epoch source is S3
   `gc/epoch_state`, process-cached (lag-only = safe). (iii) **Reconcile rebuilds only the quiesced prefix
   `Q < safe_epoch`** (§4.5), so it can never zero a live writer's tentative root edge (epoch ≥ safe_epoch > Q),
   even with a lagging read. The `e+2` limbo is sufficient independent of lag: a writer lagging by `k` publishes
   `writers/<S> = E_cur−k`, lowering `safe_epoch` to match — safety comes from quiescence, not from `+2`, not
   from any clock.
3. **Fenced single deleter**: only the lowest-seq leader deletes; every delete/epoch-advance is fence-gated.
4. **Decoupled generations + durable floor** (D2/#3): a re-create routes to `≥ floor(H)` (the durable per-hash
   generation floor), making the unconditional delete ABA-safe with only `create-if-absent` — and the floor is
   the *durable* reuse authority, so safety holds even when reclaim lags far past the recent-condemned window.

## 7. Failure handling {#failures}

| Fault | Outcome |
|---|---|
| writer crash mid-build / mid-(100GB)-upload | no ref published. A stale root `+` (logged before `setRef`) or never-`+`'d debris both bias to **over-count (leak), never loss** (§4.1 orderings) → corrected by the periodic **reconcile** (§4.5), which authoritatively rebuilds in-degree from real `refs/` (stale positive recomputes to 0; debris discovered by the `LIST`) → the usual quiescence + in-degree + fence gate reclaims. Incomplete multipart objects are reclaimed by an S3 lifecycle abort-incomplete rule (infra). Over-count only. |
| writer paused (alive lease) | its frozen pin epoch holds `safe_epoch` down; reclamation of newer epochs waits. Liveness only. |
| writer server-expired but still believes Connected | the §6.1 local-deadline self-fence forces read-only *before* expiry. Safe. |
| GC leader crash mid-round / mid-cascade | idempotent (`create-if-absent` condemned set, monotone epoch PUT, fenced deletes; re-fold recovers a partial cascade); successor takes a higher fence and re-folds. |
| split-brain leaders | only the lowest-seq leader's `sync`-ed fence passes; the stale leader fails-close. |
| total Keeper loss / restore | §4.4: safe pause → S3-only authoritative rebuild via reconcile (§4.5). No ghost-epoch hazard (epoch not in Keeper). INV-S3-COMPLETE. |
| lost/torn `gc/snap` | authoritatively rebuilt by reconcile (§4.5) from real `refs/` reachability — the high-watermark `snap/<E>` becomes the new truth; over-protective. |
| stale root positive (crashed commit/drop, §4.1) | fold over-counts → node leaks (over-count, safe); reconcile recomputes in-degree from real `refs/` → stale positive dies. Leak until next reconcile, never loss. |

## 8. Scale and performance {#scale}

- **Snapshot fold is `O(delta)`**: `gc/snap` is sharded by hash prefix; a round folds only the touched shards
  via streaming merge-sort. The only `O(all-nodes)` pass is reconcile (off-hot-path, rare).
- **Write amplification**: `+`/`-` edge deltas are coalesced (group-commit, one log object per `(shard,
  window)`); there is **no per-object hot-path write** for orphan tracking (§9). The condemned set is read once
  per epoch and cached (window ≈ `e+2`).
- **Keeper load**: `O(active writers)` tiny znodes + heartbeats; independent of pool size.
- **Big folders**: parts are small trees. A table keeps **flat refs** (D3), not one giant tree; if a single
  huge directory is ever required, shard it into a trie of trees by name-prefix.
- **Read cost**: one `GET` per node in the common `g=0` case; a node ever resurrected (`g>0`) pays
  `GET`-`404`-then-`LIST` on every read until generations are compacted (open item, §11). The part-load path must
  not trip the cold `404→LIST` en masse (§11).
- **Reconcile cost**: the periodic authoritative rebuild (§4.5) is `O(all objects)` but **`O(1)`-memory** — it
  streams the `LIST` and the `refs/`-reachability spill (both sorted by node key) through the existing merge-sort,
  never materializing a reachability set, and writes a high-watermark `snap/<E>` that retires the superseded logs.
  Reading live trees during the walk is bounded by part-count (`≪` blobs). Schedule + per-pass `LIST` budget are
  an open item (§11).

## 9. Decisions (D1–D6) and parameters {#decisions}

- **D1 — Keeper required** for coordination; the S3-only coordination mode is dropped (it had clock-skew and
  fence-steal data-loss modes). S3 remains the sole durable truth. **The epoch is NOT cached in Keeper (v1):** it
  lives only in S3 as one atomic **`gc/epoch_state {open_epoch, sealed_through}`** (A′); writers read it with a
  short process-memory TTL (§4.1). A stale local read only ever *lags* (safe), and `sealed_through` is the
  authoritative signal that forces an append re-target. This drops the fragile "Keeper-never-ahead-of-S3"
  cross-system invariant entirely (three reviewers flagged it). Keeper holds only the writer's **pin** epoch
  (`writers/<S>`, the quiescence arbiter, frozen during a build) — distinct from the local append epoch.
- **D2 — generations decoupled** from epochs (per-node resurrection counter, usually 0), with a **durable
  per-hash floor** `floors/<H>` (#3): reuse the present gen iff `g ≥ floor(H)`, else resurrect to `floor(H)`. The
  floor (= 1 + max-condemned-gen, bumped at condemn) is the **durable** reuse authority that outlives the
  bounded recent-condemned window — so a writer cannot ABA-reuse a gen whose reclaim was merely delayed. No
  `g ≥ O_W` rule; long-lived dedup preserved (a never-condemned hash has no floor object → floor 0).
- **D3 — flat refs** per part for the table active set (no giant table tree; revisit a trie only if atomic
  table snapshots become required).
- **D4 — deferred decrement cascade** on tree delete (children reclaimed over later rounds; idempotent,
  crash-safe).
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
  in-degree of every node whose support is quiesced — a *dead* writer's debris and stale positives end at
  in-degree 0 — while a *live* writer's tentative `+` (epoch ≥ safe_epoch > Q) is left untouched; the *same
  quiescence + in-degree + fence gate* reclaims what it finds.
  **Quiescence, not a wall clock, is the safety**; there is **no age-filter** (A′) — the quiesced prefix already
  protects in-flight objects, so a filter would be redundant *and* unsafe (it could skip a node whose only
  protection was a discarded `≤Q` log). Decision: **no hot-path orphan tracking; reconcile is the authoritative
  truth-maker over the quiesced prefix, the fold is the fast incremental path between reconciles.** Accepted
  cost: debris / stale positives linger until the next reconcile, an `O(objects)`-but-`O(1)`-memory pass (§11).
- **Hashes:** `cityHash128` (blob), `SipHash-128` (canonical tree entries).
- **Parameters (tuning, conservative defaults):** epoch period ≈ 10 min; `T_session` ≈ 60 s; heartbeat ≈ 15 s;
  epoch process-cache TTL ≈ 30 s (`≪` epoch period); limbo = `e+2`; reconcile cadence ≈ days (no age-filter — A′).

## 10. Verification scope {#verification}

The TLA+ model (`docs/superpowers/models/`, extended from `CaGcCore.tla`) **must** cover, under adversarial
interleaving + the failure set (session-expiry gap, split-brain, total Keeper wipe): the single-node core
(already PASS) **plus the currently-untested** (a) **multi-child commit atomicity** — a tree making a *set* of
nodes reachable at once; (b) the **deferred decrement cascade** (D4); (c) the **decoupled reuse rule** (D2,
re-verifying CE-2 under `floor`-resurrection rather than the old `e ≥ O_W` encoding); (d) the **reconcile
authoritative-rebuild path** (§4.5): a `Reconcile` action that recomputes in-degree from `refs/`-reachability —
a `Present` node unreachable from `refs` ends at in-degree 0 (debris **and** stale positives), is condemned by
the next fold, and reclaimed under the gate. Properties to check: an uploaded-but-uncommitted orphan **and** a
node with a stale root `+` are eventually reclaimed (no leak); a committed / dedup-reused / reachable object is
**never** reclaimed even if reconcile ran (its real reachability keeps in-degree > 0 — no loss); (e) the
**durable floor** (#3): after a `(H,g)` is condemned and reclaimed and old condemned records age out, a later
writer cannot reuse `(H,g)` (it sees `g < floor(H)` and resurrects) — no ABA when reclaim lags; (f) the
**pin/append split + quiesced-prefix reconcile** (P1-1): a LIVE writer with a logged-but-unpublished root edge,
possibly on a lagging read, while `Reconcile` runs — the node must never be reclaimed before commit/abort (the
frozen pin keeps `safe_epoch` low; `Reconcile`'s `Q < safe_epoch` leaves its epoch un-rebuilt). Invariants
checked: `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_ABA`, and (temporal, if feasible) no-leak-forever.

**Out of scope of the model (assumed / handled elsewhere):** the fold/snap data-plane internals (LIST
pagination, on-disk merge — implementation, not protocol); the reconcile's **physical `LIST` + tree-walk
mechanics** (the model abstracts it as `Reconcile` recomputing in-degree from `Reachable(refs)` over the
quiesced prefix); multipart-upload invisibility + S3 lifecycle abort (infra); the reader path / `404→LIST`; the
exact canonical serialization bytes and the tree-entry `size`/`attrs` (non-safety metadata); the **epoch
process-cache TTL** (modeled as a single S3 `epoch_state` that a writer may read stale-low — the
already-modeled lagging-writer case; a process read only ever lags, never leads). The edge-set log IS modeled
(idempotent add/remove keyed by `(parent,child)`); the pin/append split IS modeled. The S3-only coordination
mode is dropped (D1), so not modeled.

## 11. Open items and operational concerns {#open-items}

These do not change the protocol's safety argument but must be addressed before/within implementation; raised by
design review.

- **Observability.** Expose `system.*` so an operator can answer "I dropped a table, why didn't S3 shrink?":
  current epoch, `safe_epoch`, the oldest pinning writer (and its `O_W`), condemned-set size, reclaim backlog,
  last reconcile time, per-pool reclaimable-bytes estimate. Reclamation is intentionally lazy (limbo +
  quiescence), so this visibility is required, not optional.
- **Reconcile at scale.** The authoritative-rebuild reconcile (§4.5) is `O(all objects)` but `O(1)`-memory
  (streaming `LIST ⋈ refs/-reachability` spill through the existing merge-sort — never materialize a reachability
  set). Still to specify before implementation: the **schedule** (e.g. every N days), the **per-pass `LIST`
  budget / rate limit** (at 10¹¹ objects a full `LIST` is ~10⁸ paginated calls — money and wall-clock), and
  **per-shard checkpointing** (rebuild is per-shard, so an interrupted pass just leaves older shards on their
  previous authoritative `snap/<E′>` — always safe).
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
- **Edge-set log implementation.** The fold must compute in-degree as the count of **distinct present edges**
  keyed by `(parent,child)` (root edges by `(ref_name,node)`), so a re-added edge is idempotent (closes the
  reappend double-count without `event_id` bookkeeping) and a re-removed edge is a no-op. Confirm the on-disk
  coalesced log + merge-sort fold realize set semantics (last-writer-wins per edge key, add vs remove), not a
  signed counter. `gc/epoch_state` is a single atomic object (no separate seal namespace, no ordering race).
- **Floor storage / hot-path cost.** `floors/<H>` is sparse (only condemned hashes) and cached; confirm the reuse
  path's floor lookup stays off the critical latency path (cache + only consult on the rare resurrect/dedup-of-
  condemned case), and bound the floor object count.
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
refs       \subseteq (RefName \X Hashes \X Gen)                    \* live root edges (commit points)
edgeLog    \in [Epoch -> SUBSET (Edge \X {Add, Rem})]             \* gc/log: per-epoch ADD/REM of edges keyed by (parent,child); IDEMPOTENT
snap       \in [(Hashes \X Gen) -> Int]                            \* folded in-degree (distinct present edges); reconcile OVERWRITES ≤Q
floors     \in [Hashes -> Gen]                                     \* floors/<H> = 1+max-condemned-gen (durable; default 0); D2/#3
condemned  \in [Epoch -> SUBSET (Hashes \X Gen)]                   \* gc/condemned/<epoch>: FULL record (kind, child-edges) — model carries the set
epochState \in [open_epoch: Epoch, sealed_through: Epoch \cup {None}]  \* gc/epoch_state — ONE atomic object; SOLE epoch authority (NOT in Keeper)
\* ---- Keeper (ephemeral) ----
leaderSeq  \in [Leaders -> Nat \cup {None}]                        \* leader/lock-<seq>; lowest live = leader; seq = fence
pinEpoch   \in [Writers -> Epoch \cup {None}]                      \* writers/<S> = the QUIESCENCE PIN; FROZEN while wPending; None = no live session
\* ---- per-actor local ----
appendEpoch\in [Writers -> Epoch]                                  \* LOCAL log-append target; refreshes freely on seal (NOT in Keeper, NOT the pin)
wConn      \in [Writers -> {Connected, Disconnected}]              \* writer's belief
wSess      \in [Writers -> {SessAlive, SessExpired}]               \* server-side session truth (may differ from wConn)
wDecided   \in [Writers -> SUBSET (Hashes \X Gen)]                 \* deps decided this build
wTentEdges \in [Writers -> SUBSET Edge]                            \* this writer's flushed-but-uncommitted edges (incl. root)
wPending   \in [Writers -> BOOLEAN]                                \* an uncommitted tentative build is in flight (FREEZES pinEpoch, P1-1)
ldrConn    \in [Leaders -> {Connected, Disconnected}]
```
`epochSealed(e) == epochState.sealed_through # None ∧ e ≤ epochState.sealed_through`. **Edge-set fold:** an edge
is *present at horizon h* iff its highest-epoch op (≤ h) is `Add` (last-op-wins per `(parent,child)` key — so a
re-`Add` is idempotent and a re-`Rem` is a no-op). `inDegree(n,h)` = count of DISTINCT present edges `(_,n)`;
`snap` is this folded over sealed epochs. A fold CANDIDATE is `node[n]=Present ∧ inDegree(n)=0`.
`safeEpoch == Min({pinEpoch[w] : w live}) (or epochState.open_epoch if none)`; the quiesced-prefix watermark
`Q < safeEpoch`. **Reconcile rebuilds `snap` authoritatively from `Reachable(refs)` ONLY over epochs ≤ Q** — a
present node unreachable from `refs` with all-quiesced support → 0 (a *dead* writer's stale positive dies); a
*live* writer's tentative edge sits in an epoch `≥ safeEpoch > Q`, NOT rebuilt, so it survives. Reuse authority
is the durable `floors`: `reusable(H,g) == g ≥ floors[H]`. While `wPending[w]`, `pinEpoch[w]` is FROZEN (only
`appendEpoch[w]` may move); `W_AdvanceOW` is disabled until commit/abort.

### A.3 Init {#a-init}
All nodes `Absent`; `refs = {}`; `edgeLog/condemned` empty; `snap = 0`; `floors = [h ↦ 0]`;
`epochState = [open_epoch ↦ 0, sealed_through ↦ None]`; no leader; all `pinEpoch = None`; all `appendEpoch = 0`;
all `wPending = FALSE`; writers `Connected ∧ SessAlive` with empty decided/tentative sets.

### A.4 Actions (Next = disjunction) {#a-actions}
Each action: **guard ⇒ effect.** (`Edge` = `(parent, child)` where parent is a ref or a tree node.)

- **W_RegisterLease(w):** guard `pinEpoch[w]=None ∧ wConn[w]=Connected ∧ wSess[w]=SessAlive`.
  effect `pinEpoch[w] := epochState.open_epoch`; `appendEpoch[w] := epochState.open_epoch`.
- **W_CreateOrReuseChild(w, c):** guard writer may-act (`wConn=Connected ∧ wSess=SessAlive`). effect:
  `wPending[w] := TRUE`; let `g` be the present gen of `c`; if `node[c,g]=Present ∧ g ≥ floors[c]` → reuse (add
  `(c,g)` to `wDecided[w]`); else `node[c, floors[c]] := Present`, add `(c, floors[c])` to `wDecided[w]`
  (resurrect to the durable floor, D2/#3).
- **W_FlushEdge(w, edge):** guard `edge`'s child `∈ wDecided[w]`. effect: **if `appendEpoch[w] ≤
  epochState.sealed_through`** set `appendEpoch[w] := epochState.open_epoch` (re-target — moves ONLY the append
  epoch, NOT `pinEpoch`); then add `(edge, Add)` to `edgeLog[appendEpoch[w]]` (idempotent by edge key) and to
  `wTentEdges[w]`.
- **W_PublishRef(w, name, (T,g)):** guard may-act ∧ `(T,g)`'s edges `∈ wTentEdges[w]` ∧ **every decided child
  `(c,g_c)` still has `g_c ≥ floors[c]`** (floor re-check, P1-3 — a moved floor disables commit and forces
  `W_Abort`+rebuild, since the tree hash depends on `child_gen`). effect add `(name,T,g)` to `refs`;
  `wPending[w] := FALSE`; clear `wDecided[w]`, `wTentEdges[w]`.   (durable OUTCOME — now `W_AdvanceOW` is enabled)
- **W_Abort(w):** guard `wPending[w]=TRUE`. effect (explicit abort, P1-1): for every edge ∈ `wTentEdges[w]`
  (incl. the tentative root) add `(edge, Rem)` to `edgeLog[appendEpoch[w]]`; `wPending[w] := FALSE`; clear
  `wDecided`/`wTentEdges`. (A crash instead just expires; the tentative edges become a stale positive cleaned by
  reconcile.)
- **W_AdvanceOW(w):** guard `wConn=Connected ∧ wSess=SessAlive ∧ pinEpoch[w] < epochState.open_epoch ∧
  ¬wPending[w]` (**flush-COMMIT/ABORT-then-advance, P1-1**: advance the PIN only with no uncommitted build).
  effect `pinEpoch[w] := pinEpoch[w]+1` (and `appendEpoch[w] := Max(appendEpoch[w], pinEpoch[w])`).
- **W_Drop(w, name):** guard may-act. effect remove the ref from `refs`, then add `((ref,(T,g)), Rem)` to
  `edgeLog[appendEpoch[w]]`.
- **GC_Close(L):** guard `IsLeader(L)` (lowest live `leaderSeq`) ∧ `ldrConn[L]=Connected`. effect ATOMIC
  `epochState := [open_epoch ↦ epochState.open_epoch+1, sealed_through ↦ epochState.open_epoch]`.
- **GC_FoldCondemn(L, e):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ epochSealed(e)`. effect fold the
  edge-set ops of `edgeLog[≤e]` into `snap` (present-edge count); `condemned[e] := { n : node[n]=Present ∧
  inDegree(n)=0 }`; for each such `n=(H,g)`: `floors[H] := Max(floors[H], g+1)` (durable floor bump, #3).
- **GC_Reclaim(L, n, e_a):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ n ∈ condemned[e_a] ∧
  epochState.open_epoch ≥ e_a+2 ∧ safeEpoch > e_a ∧ inDegree(n)=0`. effect: if `n` is a tree, FIRST for each
  child edge (read from the `condemned[e_a]` record) add `(edge, Rem)` to `edgeLog[epochState.open_epoch]`
  (deferred cascade, D4), THEN `node[n] := Deleted`.
- **Reconcile(L):** guard `IsLeader(L) ∧ ldrConn[L]=Connected`. effect (authoritative rebuild over the QUIESCED
  PREFIX, P1-1 refinement 2): pick `Q < safeEpoch`; for every present `n`, set `snap[n] :=` the count of distinct
  edges `(_,n)` derived from `Reachable(refs)` whose `Add` is in an epoch `≤ Q` (so a node unreachable-from-`refs`
  with all-quiesced support → 0, cancelling a *dead* writer's stale positive); **drop `edgeLog[≤Q]`**. Ops in
  `(Q, open_epoch]` are LEFT for the incremental fold (so a live writer's tentative edge, in an epoch
  `≥ safeEpoch > Q`, is never rebuilt away). `Reachable(refs)` is the DAG closure from the live roots — the
  durable authority, NOT the pre-existing `snap`. The next `GC_FoldCondemn`/`GC_Reclaim` condemn+reclaim
  in-degree-0 nodes under the unchanged gate. (No age-filter — A′; the `Q < safeEpoch` bound already protects
  every in-flight object.)
- **Failures:** `Sess_Expire(w)` (`wSess[w]:=SessExpired ∧ pinEpoch[w]:=None ∧ wPending[w]:=FALSE` — the build is
  abandoned by death; its tentative edges remain a stale positive cleaned by reconcile, NOT explicitly `Rem`'d);
  `Disconnect(w)`/`Reconnect(w)`; `Ldr_Disconnect(L)`/`Ldr_Steal(L)` (new lowest seq); `Keeper_Wipe` (all
  `pinEpoch:=None ∧ wPending:=FALSE`, all `leaderSeq:=None`, all writers→read-only; **`edgeLog`/`floors`/`snap`/
  `epochState` are S3, NOT cleared**); `Recover` (purge `leader/`+`writers/` only — **no epoch znode to purge**;
  elect, atomic `epochState` bump+seal, re-register).

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
`epochState.open_epoch ≤ MaxEpoch+2`. (No `RetentionEps` — A′ has no age-filter.) Bounded model checking —
strong evidence within bounds, not a proof.

**Scenarios that MUST be checked:**
- **No-leak (debris):** `W_CreateOrReuseChild` then `Sess_Expire` *before* `W_PublishRef` (uploaded, never
  committed) → `Reconcile` sets its `snap`=0 (unreachable) → condemned → `GC_Reclaim` deletes it.
- **No-leak (stale positive):** a `ref→(T,g)` `+` is logged (step 5) but the writer dies before `W_PublishRef`
  (no ref) → fold over-counts `T` → `Reconcile` recomputes `snap[T]=0` (T ∉ Reachable(refs)) → T reclaimed.
- **No-loss (P1-1, the headline):** a LIVE writer logs the `(ref,(T,g))` edge, has NOT published the ref, and
  `Reconcile` runs (possibly with the writer on a lagging read, and with `appendEpoch` having moved ahead). `T`
  must **never** be `Deleted` before the writer commits or aborts: the frozen `pinEpoch` keeps `safeEpoch` at the
  build epoch, and `Reconcile`'s `Q < safeEpoch` watermark leaves `T`'s epoch un-rebuilt. Drive every
  interleaving of `W_FlushEdge`/`W_AdvanceOW`/`Reconcile`/`GC_Reclaim` against an uncommitted live build — this
  is the case that was broken (W_FlushEdge moving `appendEpoch` must NOT move `pinEpoch`).
- **No-loss (reachable):** a reachable node (committed / dedup-reused) is never `Deleted`, even after `Reconcile`.
- **No-ABA-when-reclaim-lags (floor):** condemn `(H,0)` (bumps `floors[H]=1`), delay its reclaim past the
  recent window, then a new writer wants `H` → it must resurrect to `floors[H]` (=1), never reuse `(H,0)`.
- **Floor-move-before-commit (P1-3):** a reused child's `floor` moves after its parent tree was flushed →
  `W_PublishRef` is disabled (re-check fails) → the writer must `W_Abort` (rebuild) → no ref ever names a
  below-floor child.
