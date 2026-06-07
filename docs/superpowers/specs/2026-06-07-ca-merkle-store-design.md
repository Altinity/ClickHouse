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
(moved to `reports/obsolete/`). Decisions D1–D5 are settled (§9). This document is written to be converted to a
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
- [9. Decisions (D1–D5) and parameters](#decisions)
- [10. Verification scope](#verification)
- [Appendix A. Formal model (TLA+-convertible)](#appendix)

---

## 1. Overview and goals {#overview}

The content-addressed (CA) backend is a `metadata_type = content_addressed` for an object-storage disk. A plain
non-replicated `MergeTree` on such a disk needs no engine or DDL change. The on-store model is **"Git for
MergeTree": a Merkle DAG of immutable folders.** Files are content-addressed blobs; folders (including every
MergeTree part) are content-addressed trees; the only mutable objects are **refs**, which are the GC roots and
the commit points. Reclamation is a **lock-free, Keeper-coordinated, S3-durable Epoch-Based-Reclamation (EBR)
garbage collector**: writers and the background sweep never share a mutex; a live writer holds reclamation back
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
the GC condemns generation `g` and a writer needs that content again and recreates it at `g+1`. `g` is
**decoupled from the epoch**; it exists only so a GC delete of `(N,g)` and a concurrent re-create can never
collide on a key (the re-create goes to `(N,g+1)`, a different key — G2/ABA).

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
  gc/epoch                                  DURABLE current epoch E_cur (ONE object; only the fenced leader writes it; Keeper
                                              caches it for hot reads — §3.2. S3 stays authoritative)
  gc/log/<epoch>/<shard>/<event_id>        coalesced EDGE deltas (+/-): ref→(T,g), (T,g)→child(h,g), and lease(S)→(h,g) in-flight pins
                                              (D6); hash-prefix sharded; event_id-deduped
  gc/snap/<epoch>/<shard>                   folded per-node IN-DEGREE counts, sorted by node key, sharded
  gc/condemned/<epoch>                      ONE immutable object/round = the (node,gen) set condemned that round (= reuse-check input,
                                              reclaim work-list, and durable condemn record; replaces per-object tombstones + a sealed index)
  _pool_meta                                pool identity / format version
```
Notes: all mutable roots live under `roots/<server_id>/` and **mirror the real ClickHouse path** (`store/<uuid>`
for live tables, `shadow/<backup_id>/store/<uuid>` for frozen backups) so the metadata layer maps onto it
directly. There is **no per-object tombstone** — condemnation is the per-epoch `gc/condemned/<e>` set. There is
**no `active` hint**: a node's present generation is resolved by a `GET <hash>/g=0` first (the **>90% case** —
`g` stays `0`), and only on `404` a `LIST <hash>/` (a `LIST` costs ≈10× a `GET`, so this is a **cold** path, not
the hot path). Dropping the `active` hint removes the whole class of races around keeping it in sync, paying only
that rare cold `LIST`. Condemnation gates *reuse/attachment*, never *reads*.

### 3.2 Keeper (ephemeral coordination only, `O(active writers)`) {#layout-keeper}
```
/clickhouse/ca/<pool>/
  epoch                 current epoch E_cur — a DERIVED CACHE of the durable S3 gc/epoch (the leader writes S3 FIRST,
                        then refreshes this; writers READ their O_W from here, avoiding a per-commit S3 GET)
  leader/lock-<seq>     EPHEMERAL-SEQUENTIAL; lowest live seq = GC leader; <seq> IS the monotone fence token
  writers/<session_id>  EPHEMERAL; data = that writer's observed epoch O_W (one integer)
```
Nothing here is authoritative durable state and nothing scales with object count. The `epoch` znode is a
**rebuildable cache** of the S3 `gc/epoch` (S3 stays the authority); because the leader writes S3 before
refreshing it, Keeper is never *ahead* of S3 — at worst equal-or-behind, the safe direction (a writer can only
*lag* the epoch, already covered by quiescence + closed-epoch reappend). Losing it loses nothing: recovery
re-reads `gc/epoch` from S3. This is what keeps a Keeper wipe non-destructive (`INV-S3-COMPLETE`).

## 4. Protocols {#protocols}

### 4.1 Writer — commit part `name` {#proto-write}
1. **Lease.** Hold a live Keeper session `S`; `O_W :=` the Keeper `epoch` cache (no per-commit S3 `GET`; §3.2);
   publish `writers/<S> = O_W`. **Self-fence (D1 hinge):** a commit may proceed only while
   `Connected ∧ local_elapsed_since_renew < T_session − margin`; otherwise the writer goes **read-only**.
   (`Disconnected`-detection alone is insufficient — TLC CE-4.)
2. **Condemned cache.** Read `gc/condemned/<e>` for the last ~`e+2` epochs once each (immutable closed-epoch
   sets); cache. `condemned?(node,g)` is a membership test against this cache.
3. **Build locally.** For each file `f`: `Bf := cityHash128(f)` (D5: from `checksums.txt` for part files);
   `createIfAbsent blobs/<Bf>/<g>`; upload once if absent. For each subdir: recurse → child tree.
   **Reuse** the present generation `(hash,g)` iff present ∧ `¬condemned?(hash,g)`; else **resurrect**:
   `createIfAbsent` at `gen+1`. **Pin (D6):** for every node touched (uploaded or reused) append a
   `lease(S) → (hash,g)` pin edge to `gc/log/<O_W>/<shard>` — the live lease is a transient GC root, so an
   uploaded node is **never fold-invisible** while the build is in flight. This closes the upload-crash orphan
   gap: a crash after upload but before the step-5 `+` still leaves the blob enumerable to the fold.
4. **Tree.** `T := SipHash-128(canonical entries)`; `createIfAbsent trees/<T>/<g>`. (`part_id ≡ T`.)
5. **Edge `+`.** Append edge `+` deltas to `gc/log/<O_W>/<shard>`: `ref→(T,g)` and `(T,g)→each child(h,g)`.
   Make them **durable**, and only **then** may `O_W` advance — **flush-`+`-then-advance** (TLC CE-1). After the
   `+` is durable, **re-check** `condemned?` for reused children; if any is now condemned, **resurrect to
   `gen+1` and append a `-` for the abandoned edge** (dedup-preserving reuse; resolves CE-2 under D2).
6. **Commit.** `setRef roots/<server_id>/store/.../refs/<name> = RefPayload{T,g,header}` — the commit point,
   **written last** (re-check the self-fence immediately before). Once published, the durable tree edges hold
   every node, so append `-` deltas retracting this build's `lease(S)` pins (D6).
7. **Drop.** `removeRef` **first**, then append the `ref→(T,g)` `-` delta.

A crash before step 6 leaves the part not-live; its nodes age out once the writer's lease is gone and their
in-degree folds to 0 (over-count only).

### 4.2 GC leader — one round (fenced single deleter) {#proto-gc}
Guard every step: I am the lowest-seq child of `leader/` (a **`sync`-ed** read) ∧ `Connected`; else **fail-close
(stop)**. A **new** leader re-folds + re-quiesces under its **own** fence before any delete.
```
R0 CLOSE    E := GET gc/epoch (S3); fenced PUT gc/epoch = E+1 in S3 (DURABLE FIRST), THEN refresh the Keeper
            `epoch` cache to E+1.   (rotation barrier; writers re-sync to E+1 and REAPPEND any still-needed
                                     in-flight `+` into E+1. Keeper ≤ S3 always — the safe direction.)
R1 FOLD     CLOSED epochs only: per shard, streaming merge-sort gc/log/<≤E>/<shard> ⋈ gc/snap/<…>/<shard>
            → gc/snap/<E+1>/<shard>; per-node IN-DEGREE; emit in-degree-0 CANDIDATES.   (O(delta), sharded)
            A lease(S)→(h,g) pin counts toward in-degree ONLY while session S is live (S ∈ getChildren(writers/),
            sync-ed); a DEAD lease's pins are dropped, so a crashed build's orphan nodes fold to in-degree 0 and
            are reclaimed by this same path — NO full scan (D6).
R2 CONDEMN  write the candidate (node,gen) set to gc/condemned/<e_a> as ONE object (fenced; e_a = folded epoch).
R3 QUIESCE  safe_epoch := min(O_W) over getChildren(writers/) [sync-ed]; if none → E_cur.
R4 RECLAIM  for each (N,e_a) in gc/condemned/<e_a> with
              E_cur ≥ e_a+2  ∧  safe_epoch > e_a  ∧  in-degree==0  ∧  still-leader(fence):
                DELETE the node object;  if N is a TREE, append `-` deltas for ITS child edges
                (DEFERRED CASCADE — children whose in-degree reaches 0 are condemned in a LATER round, D4).
```
`E_cur ≥ e_a+2` = Crossbeam 3-epoch limbo; `safe_epoch > e_a` = EBR/QSBR quiescence; the fence gates every
DELETE. `Retention` is **not** on this fast path (quiescence covers it) — it is a backstop only for reconcile
(§7).

### 4.3 Reader {#proto-read}
`GET ref → (T,g)`; `resolveNode(x,g)`: `GET <x>/<g>`, on `404` `LIST <x>/` and read any present generation;
walk tree entries, recurse into child trees, `GET` child blobs (`404 → LIST` at each node). A present-but-
condemned node still reads correctly (condemnation blocks only new reuse/attach).

### 4.4 Recovery — total Keeper loss {#proto-recovery}
```
during:  every writer session drops → READ-ONLY (self-fence); the leader's election znode is gone → GC stops.
         No mutation that could dangle or lose data occurs while Keeper is down.  SAFE PAUSE.
restore (even empty Keeper):
         PURGE any backup-restored ghost leader//writers/ znodes;  elect a leader;
         E := GET gc/epoch (S3);  fenced PUT gc/epoch = E+1  (fence off pre-outage in-flight epochs);  refresh
         the Keeper `epoch` cache;  writers reconnect, re-create writers/<S>, observe the new epoch, resume.
         If gc/snap integrity is in doubt → RECONCILE: full DAG reachability from refs/ (durable, written-last
         authority) rebuilds gc/snap; reconcile NEVER reclaims anything younger than Retention.  S3 ALONE SUFFICES.
```

## 5. Invariants and the safety argument {#invariants}

- **INV-NO-LOSS:** a node `(N,g)` is deleted only when unreferenced in the fold **and** `safe_epoch > e_a`
  **and** `E_cur ≥ e_a+2` **and** the leader still holds its fence. EBR quiescence (`safe_epoch > e_a` ⇒ every
  live writer has observed past `e_a`, so it has the condemned set and will not newly reference `(N,g)`),
  combined with the fold showing in-degree 0, makes "no future reference can appear" hold at the delete point.
- **INV-NO-DANGLE:** a published ref always resolves to present bytes (reads tolerate a stale generation via
  `LIST`).
- **INV-NO-ABA:** a node once deleted is never returned to present at the **same** key — a re-create uses
  `gen+1` (D2), a different key; only `create-if-absent` is needed.
- **INV-OVER-COUNT-ONLY:** every failure mode (lost/dup/reordered `+`/`-`, crash, partial cascade) biases to
  over-count (a node kept longer → a leak, reconciled later), never to under-count (loss). The reverse index is
  a *sloppy candidate filter*, not the delete authority; the authority is quiescence + in-degree + fence.
  Upload-crash orphans (a blob PUT whose tree was never committed) are kept alive by the writer's `lease(S)` pin
  and reclaimed when the lease dies (D6); a periodic Retention-guarded orphan-sweep (§7) is the backstop for
  anything the delta path cannot enumerate.
- **INV-S3-COMPLETE:** S3 alone determines and rebuilds the full state; Keeper holds only ephemeral coordination
  plus **rebuildable caches** (the `epoch` znode) — never authoritative durable state; total Keeper loss loses
  nothing (the epoch is re-read from the durable S3 `gc/epoch`).
- **Liveness (no-leak-forever):** every genuinely unreachable node is eventually condemned (in-degree 0 fold)
  and, after the limbo + quiescence, reclaimed; deferred cascades drain dead subtrees over successive rounds; a
  stuck writer cannot stall reclamation forever (its lease expires → it is dropped from `safe_epoch`).

## 6. The hinges (assumptions correctness rests on) {#hinges}

1. **Writer self-fence on the Keeper session** (D1): a consequential op (reuse-commit) requires
   `Connected ∧ session-alive-by-local-clock` — fail-stop on `Disconnected`, deadline strictly inside
   `T_session`. This is a session-timeout assumption (no inter-clock skew, because Keeper's session is the
   single arbiter). The leader likewise fail-closes on `Disconnected`, with a `sync`-ed lowest-seq fence
   re-check before every delete.
2. **`flush-+-then-advance`**: a writer makes an edge-`+` durable in S3 before advancing `O_W` in Keeper
   (Keeper linearizability does **not** extend to S3). The leader folds only **closed** epochs; writers reappend
   in-flight `+` into the open epoch. Symmetrically, the leader writes the durable S3 `gc/epoch` **before**
   refreshing the Keeper `epoch` cache, so the cache is never *ahead* of the durable epoch — a writer can only
   *lag*, the safe direction already covered by quiescence + closed-epoch reappend.
3. **Fenced single deleter**: only the lowest-seq leader deletes; every delete/epoch-advance is fence-gated.
4. **Decoupled generations** (D2): re-create routes to `gen+1`, making the unconditional delete ABA-safe with
   only `create-if-absent`.

## 7. Failure handling {#failures}

| Fault | Outcome |
|---|---|
| writer crash mid-build / mid-(100GB)-upload | no ref published; nodes already uploaded are pinned to the writer `lease(S)` (D6), so they stay **fold-visible**; the lease lapses → pins drop → nodes fold to in-degree 0 → reclaimed by the normal path (no full scan). Incomplete multipart objects are reclaimed by an S3 lifecycle abort-incomplete rule (infra). Over-count only. |
| stray orphan the delta path can't enumerate (lost pin / torn log) | reclaimed by the periodic **orphan-sweep**: a Retention-guarded full `LIST` of `blobs/`+`trees/` minus the fold/reachability set (the Iceberg `remove_orphan_files` / `git gc` model); rare, off-hot-path; never deletes anything younger than `Retention`. |
| writer paused (alive lease) | pins `safe_epoch` at `O_W`; reclamation of newer epochs waits. Liveness only. |
| writer server-expired but still believes Connected | the §6.1 local-deadline self-fence forces read-only *before* expiry. Safe. |
| GC leader crash mid-round / mid-cascade | idempotent (`create-if-absent` condemned set, monotone epoch PUT, fenced deletes; re-fold recovers a partial cascade); successor takes a higher fence and re-folds. |
| split-brain leaders | only the lowest-seq leader's `sync`-ed fence passes; the stale leader fails-close. |
| total Keeper loss / restore | §4.4: safe pause → S3-only rebuild via reconcile (honoring Retention). INV-S3-COMPLETE. |
| lost/torn `gc/snap` | rebuilt by reconcile (the only full DAG scan); over-protective. |

## 8. Scale and performance {#scale}

- **Snapshot fold is `O(delta)`**: `gc/snap` is sharded by hash prefix; a round folds only the touched shards
  via streaming merge-sort. The only `O(all-nodes)` pass is reconcile (off-hot-path, rare).
- **Write amplification**: `+`/`-` edge deltas are coalesced (group-commit, one log object per `(shard,
  window)`). The condemned set is read once per epoch and cached (window ≈ `e+2`, not Retention).
- **Keeper load**: `O(active writers)` tiny znodes + heartbeats; independent of pool size.
- **Big folders**: parts are small trees. A table keeps **flat refs** (D3), not one giant tree; if a single
  huge directory is ever required, shard it into a trie of trees by name-prefix.

## 9. Decisions (D1–D5) and parameters {#decisions}

- **D1 — Keeper required** for coordination; the S3-only coordination mode is dropped (it had clock-skew and
  fence-steal data-loss modes). S3 remains the sole durable truth. Keeper additionally caches the current epoch
  as a derived, rebuildable znode so writers avoid a per-commit S3 `GET` (written S3-first, so never ahead of
  the durable epoch).
- **D2 — generations decoupled** from epochs (per-node resurrection counter, usually 0); reuse = present-gen
  if not condemned else resurrect to `gen+1`; no `g ≥ O_W` rule; long-lived dedup preserved.
- **D3 — flat refs** per part for the table active set (no giant table tree; revisit a trie only if atomic
  table snapshots become required).
- **D4 — deferred decrement cascade** on tree delete (children reclaimed over later rounds; idempotent,
  crash-safe).
- **D5 — trust `checksums.txt`** for part-trees: blob hash = `cityHash128` so copy/fetch/ATTACH build the
  part-tree and dedup from `checksums.txt` with no re-read/re-hash; fail-safe re-hash on mismatch.
- **D6 — in-flight lease pins + periodic orphan-sweep.** A writer pins every node it uploads/reuses to its
  Keeper `lease(S)` (a transient GC root), so write-crash orphans stay **fold-visible** and are reclaimed when
  the lease dies — the `O(delta)` fold needs no full `LIST` for the common crash case. A periodic
  Retention-guarded full-`LIST` orphan-sweep (the Iceberg / `git gc` model) is the backstop for anything the
  delta path cannot enumerate. This matches the PoC's existing in-flight-pinned-blobs mechanism.
- **Hashes:** `cityHash128` (blob), `SipHash-128` (canonical tree entries).
- **Parameters (tuning, conservative defaults):** epoch period ≈ 10 min; `T_session` ≈ 60 s; heartbeat ≈ 15 s;
  limbo = `e+2`; `Retention` = reconcile backstop (e.g. hours/days, ≥ max op duration).

## 10. Verification scope {#verification}

The TLA+ model (`docs/superpowers/models/`, extended from `CaGcCore.tla`) **must** cover, under adversarial
interleaving + the failure set (session-expiry gap, split-brain, total Keeper wipe): the single-node core
(already PASS) **plus the currently-untested** (a) **multi-child commit atomicity** — a tree making a *set* of
nodes reachable at once; (b) the **deferred decrement cascade** (D4); (c) the **decoupled reuse rule** (D2,
re-verifying CE-2 under `gen+1` resurrection rather than the old `e ≥ O_W` encoding); (d) the **in-flight lease
pin + dead-lease retraction** (D6) — an uploaded-but-uncommitted node must stay reachable while the lease lives
and be reclaimed (not leaked, not lost) once it dies. Invariants checked: `INV_NO_LOSS`, `INV_NO_DANGLE`,
`INV_NO_ABA`, and (temporal, if feasible) no-leak-forever.

**Out of scope of the model (assumed / handled elsewhere):** the fold/snap data-plane internals (signed counts,
LIST pagination, torn snapshot, `event_id` dedup — implementation correctness, not protocol); multipart-upload
invisibility + S3 lifecycle abort (infra); the reader path / `404→LIST`; the exact canonical serialization
bytes and the tree-entry `size`/`attrs` (non-safety metadata); the S3-vs-Keeper epoch cache split (modeled as a
single `epochCur` — the S3-first write order means Keeper only ever lags, which is the already-modeled
lagging-writer case); and the S3-only coordination mode (dropped per D1, so not modeled).

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
RetentionEps  reconcile retention in epochs (model: 0 unless modeling reconcile)
```

### A.2 Variables (state) {#a-vars}
```
\* ---- S3 (durable) ----
node       \in [Hashes \X Gen -> {Absent, Present, Deleted}]      \* object existence per (hash,gen)
refs       \subseteq (RefName \X Hashes \X Gen)                    \* live root edges (commit points)
plus       \in [Epoch -> SUBSET Edge]                              \* gc/log +deltas per epoch (Edge = root edge or tree->child edge)
minus      \in [Epoch -> SUBSET Edge]                              \* gc/log -deltas per epoch
snap       \in [(Hashes \X Gen) -> Int]                            \* folded in-degree (sloppy filter)
condemned  \in [Epoch -> SUBSET (Hashes \X Gen)]                   \* gc/condemned/<epoch>
epochCur   \in Epoch                                               \* gc/epoch (authoritative)
\* ---- Keeper (ephemeral) ----
leaderSeq  \in [Leaders -> Nat \cup {None}]                        \* leader/lock-<seq>; lowest live = leader; seq = fence
writerOW   \in [Writers -> Epoch \cup {None}]                      \* writers/<S> observed epoch; None = no live session
leasePins  \in [Writers -> SUBSET (Hashes \X Gen)]                 \* lease(S)->(h,g) in-flight pins; transient roots (D6)
\* ---- per-actor local ----
wConn      \in [Writers -> {Connected, Disconnected}]              \* writer's belief
wSess      \in [Writers -> {SessAlive, SessExpired}]               \* server-side session truth (may differ from wConn)
wDecided   \in [Writers -> SUBSET (Hashes \X Gen)]                 \* deps decided but not yet +durable
wPinDurable\in [Writers -> SUBSET Edge]                            \* this writer's durable +edges (flush-then-advance)
ldrConn    \in [Leaders -> {Connected, Disconnected}]
```
`epochClosed(e) == e < epochCur`. `inDegree(n)` is the leader's fold over CLOSED epochs only (the closed-epoch
barrier): `snap[n]` plus the closed-epoch `plus`/`minus` net for `n`, **plus 1 for each live-writer `leasePins`
holding `n`** (a dead lease's pins do not count — D6 orphan reclamation).

### A.3 Init {#a-init}
All nodes `Absent`; `refs = {}`; `plus/minus/condemned` empty; `snap = 0`; `epochCur = 0`; no leader; all
`writerOW = None`; all `leasePins = {}`; writers `Connected ∧ SessAlive` with empty decided/pin sets.

### A.4 Actions (Next = disjunction) {#a-actions}
Each action: **guard ⇒ effect.** (`Edge` = `(parent, child)` where parent is a ref or a tree node.)

- **W_RegisterLease(w):** guard `writerOW[w]=None ∧ wConn[w]=Connected ∧ wSess[w]=SessAlive`.
  effect `writerOW[w] := epochCur`.
- **W_CreateOrReuseChild(w, c):** guard writer may-act (`wConn=Connected ∧ wSess=SessAlive`). effect: if
  `node[c,g]=Present ∧ (c,g) ∉ UNION condemned[..]` for the present `g` → reuse (add `(c,g)` to `wDecided[w]`,
  `leasePins[w]`); else `node[c, g+1] := Present`, decide `(c,g+1)`, pin it (`leasePins[w]`) — resurrect, D2+D6.
- **W_FlushPlus(w, edge):** guard `edge`'s child `∈ wDecided[w]`. effect add `edge` to `plus[writerOW[w]]` and to
  `wPinDurable[w]` (durable). **Only after this may O_W advance.**
- **W_AdvanceOW(w):** guard `wConn=Connected ∧ wSess=SessAlive ∧ writerOW[w] < epochCur ∧ wDecided[w] ⊆
  {children with their +edge ∈ wPinDurable[w]}`  (flush-+-then-advance, CE-1/CE-2). effect
  `writerOW[w] := writerOW[w]+1`; re-check: any decided child now in `condemned` → resurrect to `g+1`, add its
  `-` to `minus`.
- **W_PublishRef(w, name, (T,g)):** guard may-act ∧ `(T,g)`'s `+` edges `∈ wPinDurable[w]`. effect add
  `(name,T,g)` to `refs` (commit point); `leasePins[w] := {}` (durable tree edges now hold every node — retract
  pins, D6).
- **W_Drop(w, name):** guard may-act. effect remove the ref from `refs`, then add the `ref→(T,g)` `-` to `minus`.
- **GC_Close(L):** guard `IsLeader(L)` (lowest live `leaderSeq`) ∧ `ldrConn[L]=Connected`. effect
  `epochCur := epochCur+1`.
- **GC_FoldCondemn(L, e):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ epochClosed(e)`. effect fold `plus[≤e]`,
  `minus[≤e]` into `snap`; `condemned[e] := { n : inDegree(n)=0 }`.
- **GC_Reclaim(L, n, e_a):** guard `IsLeader(L) ∧ ldrConn[L]=Connected ∧ n ∈ condemned[e_a] ∧ epochCur ≥ e_a+2 ∧
  (min over live writers of writerOW) > e_a ∧ inDegree(n)=0`. effect `node[n] := Deleted`; if `n` is a tree, for
  each child edge add its `-` to `minus[epochCur]` (deferred cascade, D4).
- **Failures:** `Sess_Expire(w)` (`wSess[w]:=SessExpired ∧ writerOW[w]:=None ∧ leasePins[w]:={}` — pins drop
  with the lease, exposing orphans to the fold, D6; independent of `wConn`); `Disconnect(w)`/`Reconnect(w)`;
  `Ldr_Disconnect(L)`/`Ldr_Steal(L)` (new lowest seq); `Keeper_Wipe` (all `writerOW:=None`, all
  `leasePins:={}`, all `leaderSeq:=None`, all writers→read-only); `Recover` (purge, elect, bump epoch,
  re-register).

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
`Writers = {w1,w2}`, `Leaders = {L1,L2}`, `MaxEpoch = 2`, `MaxGen = 1`, a tree with 2 children (multi-child),
`RetentionEps = 0`; `StateConstraint` caps `|refs| ≤ 2`, per-writer `wDecided`/`wPinDurable`/`leasePins` ≤ 2,
fence ≤ MaxEpoch+2. The D6 orphan case is exercised by `W_CreateOrReuseChild` then `Sess_Expire` *before*
`W_PublishRef` (uploaded, pinned, never committed) — TLC must show the node is reclaimed (no leak) and never
read as a live ref (no loss). Bounded model checking — strong evidence within bounds, not a proof.
