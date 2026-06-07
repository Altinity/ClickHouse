# CA storage — concrete S3 + Keeper layout and protocols

The consolidated current design: a **Merkle DAG of immutable folders** (`2026-06-07-ca-merkle-folders-design.md`)
reclaimed by the **Keeper-only EBR GC** (`2026-06-07-ca-gc-keeper-only-profile.md`, TLC-checked core in
`docs/superpowers/models/`). This doc shows the exact object/znode layout and the protocols.

## 1. State split — `INV-S3-COMPLETE` {#split}

- **S3** holds ALL durable state: the Merkle DAG (blobs, trees, refs) + the GC index (epoch, log, snap,
  condemned sets). Self-describing; the full state is understandable and rebuildable from S3 alone.
- **Keeper** holds ONLY ephemeral coordination, size **O(active writers + contending leaders)** — never
  O(objects). Losing Keeper (even wiping it) loses no durable state: writers self-fence read-only, GC stops,
  and on restore everything resumes from S3.

## 2. S3 layout {#s3}

One pool = one disk root. `<H>`/`<T>` are content hashes; `<g>` is a **per-node resurrection generation** —
`0` until the GC condemns generation `g` and a writer recreates the content at `g+1` (D2: decoupled from the
epoch; stays `0` in the common case). Generations exist only so a GC delete and a concurrent re-create never
collide on the same key (ABA-safety).

```
<pool>/
  blobs/<H[:2]>/<H>/<g>                      immutable FILE bytes — a DAG leaf node (H,g)
  trees/<T[:2]>/<T>/<g>                       immutable FOLDER — canonical serialized entries
                                                [(name, kind∈{blob,tree}, child_hash, child_gen)], node (T,g)
  store/<server_id>/<uuid[:3]>/<uuid>/
      refs/<part_name>                        LIVE REF -> RefPayload{ tree_hash T, gen, header } — GC ROOT,
                                                the COMMIT POINT (written last, removed first)
      refs/<part_name>.meta                   mutable per-part sidecar files (verbatim)
      refs/detached/<name>                    detached ref (GC root, not in the active set)
      files/<tail>                            table-level verbatim files (format_version.txt, …)
  shadow/<backup>/.../refs/<part>            FROZEN ref — GC root, table-lifetime-independent
  gc/epoch                                    authoritative current epoch E_cur (ONE object; only the fenced
                                                leader writes it — this is what makes a Keeper wipe non-destructive)
  gc/log/<epoch>/<shard>/<event_id>          coalesced EDGE deltas (+/-): `ref→(T,g)` and `(T,g)→child(hash,g)`,
                                                (hash,gen)-resolved; sharded by hash prefix; event_id-deduped
  gc/snap/<epoch>/<shard>                     folded per-node IN-DEGREE counts, sorted by node key, sharded
  gc/condemned/<epoch>                        ONE immutable object per round = the set of (node,gen) condemned
                                                in that epoch. Replaces per-object tombstones AND a separate
                                                sealed index. Written once by the fenced leader when it folds
                                                the epoch; the writer's reuse-check input; the reclaim work-list.
  _pool_meta                                  pool identity / format version
```

Notes: a **node** is a blob or a tree. **Condemnation is recorded per epoch, not per object** — there is no
`<…>.tombstone` object; "is `(H,g)` condemned?" is a membership test against the cached `gc/condemned/<e>` sets
(see §4). There is **no `active` hint** — a reader resolves a node's present generation by trying `g=0` and
falling back to `LIST <H>/`. Condemnation gates *reuse/attachment*, never *reads*. `part_id` ≡ the part-tree
hash `T`. Refs/`.meta`/`files` live under one `refs/`+`files/` prefix so a ref-scoped delete reclaims them and
the DAG reachability sweep (over `blobs/`+`trees/`) never has to see them.

**Why per-epoch, not per-object or in Keeper:** per-object tombstones explode the S3 object count to
`O(condemned)` and force a `HEAD` per reused node (write-path round-trips); Keeper is wrong because condemn
state is *durable* (it must survive a Keeper wipe — `INV-S3-COMPLETE`) and would bloat Keeper to
`O(condemned)`, breaking the `O(active-writers)` rule. The per-epoch set is one small immutable object the
writer reads **once and caches**; its relevant window is just the **`e+2` limbo** (a condemned-but-undeleted
node is either deleted within ~2 epochs or has become referenced again, hence safe to reuse) — **not** the
retention window (which only governs the rare reconcile scan). No `RECOVER`/un-condemn is needed in EBR
(a writer that sees a node condemned resurrects to a fresh generation), so the set being immutable is fine.

## 3. Keeper layout {#keeper}

Ephemeral only. One subtree per pool. Nothing here is durable; nothing scales with object count.

```
/clickhouse/ca/<pool>/
  leader/
    lock-<seq>          EPHEMERAL-SEQUENTIAL; the lowest live seq is the GC leader; its <seq> IS the fence token
  writers/
    <session_id>        EPHEMERAL; data = the writer's observed epoch O_W (a single integer)
```

Leader election = "I am the lowest-seq child of `leader/`"; the sequence number is a free monotone fence.
A writer's lease = its ephemeral session; `writers/<S>` publishes its `O_W`. The GC reads `getChildren(writers/)`
(with `sync`) to compute `safe_epoch = min O_W`. `gc/epoch` is **not** mirrored here — S3 is authoritative.

## 4. Write protocol (commit part `name`) {#write}

```
0. Keeper session S live; O_W := GET gc/epoch (S3); writers/<S> := O_W. SELF-FENCE: a commit may proceed only
   while Connected ∧ local_elapsed_since_renew < T_session − margin; otherwise GO READ-ONLY.
0b. CONDEMNED CACHE: read gc/condemned/<e> for the last ~(e+2) epochs once each (immutable closed-epoch sets),
    cache for this epoch. "condemned?(node,e)" is a membership test against this cache.
1. Build the part locally. For each file f: Bf := hash(f); putBlob -> createIfAbsent blobs/<Bf>/<g>; upload once.
   For each subdir: recurse -> child tree hash. DEDUP-REUSE the present generation (hash,g) iff present AND
   NOT condemned?(hash,g); else RESURRECT (createIfAbsent at gen+1). (No `g >= O_W` rule — D2 decoupled.)
2. T := canonical-serialize(entries); createIfAbsent trees/<T>/<g>.   (part_id == T)
3. Append EDGE `+` deltas to gc/log/<O_W>/<shard>:  ref→(T,g)  and  (T,g)→each child(hash,e).
   Make them DURABLE, then (and only then) may O_W advance — FLUSH-`+`-THEN-ADVANCE. After the edge-`+` is
   durable, RE-CHECK condemned?(child) against the refreshed condemned cache; if now condemned, resurrect to
   gen+1 AND append the `-` for the abandoned edge (the dedup-preserving reuse rule; CE-2 resolved by D2).
4. setRef store/.../refs/<name> = RefPayload{T,g,header}.   COMMIT POINT — written LAST; recheck self-fence first.
5. Drop (later): removeRef FIRST, then append the ref→(T,g) `-` delta.
```
Crash before step 4 ⇒ part not live; its nodes age out once the writer's lease is gone and their in-degree
folds to 0. Over-count only.

## 5. Read protocol {#read}

```
GET ref -> {T,g}.  resolveNode(x,g): GET <x>/<g>; on 404, LIST <x>/ and read any present generation.
Walk the tree's entries; recurse into child trees, GET child blobs. 404 → LIST fallback at each node.
A present-but-condemned node still reads correctly (condemnation only blocks new reuse/attach).
```

## 6. GC round protocol (fenced leader) {#gc}

```
GUARD every step: I am the lowest-seq child of leader/ (sync-ed read) ∧ Connected; else FAIL-CLOSE (stop).
A NEW leader re-folds + re-quiesces under its own fence before any delete.

R0 CLOSE    E := GET gc/epoch;  fenced PUT gc/epoch = E+1.   (writers re-sync to E+1; in-flight `+` reappends there)
R1 FOLD     CLOSED epochs only: per shard, streaming merge-sort gc/log/<≤E>/<shard> ⋈ gc/snap/<…>/<shard>
            → new gc/snap/<E+1>/<shard>; per-node IN-DEGREE; emit in-degree-0 CANDIDATES.   (O(delta), sharded)
R2 CONDEMN  write the candidate set (the in-degree-0 nodes of this fold) to gc/condemned/<e_a> as ONE object
            (fenced PUT; e_a = the folded epoch). No per-object tombstones.
R3 QUIESCE  safe_epoch := min(O_W) over getChildren(writers/) [sync-ed]; if none → E_cur.
R4 RECLAIM  for each (N,e_a) listed in gc/condemned/<e_a> with
              E_cur ≥ e_a+2  ∧  safe_epoch > e_a  ∧  in-degree==0  ∧  still-leader(fence):
                DELETE the node object;  if N is a TREE, append `-` deltas for ITS child edges (DECREMENT
                CASCADE → children whose in-degree reaches 0 are condemned in the current round).
            (No tombstone to drop: a deleted node's entry simply ages out of the writer's ~e+2 cache window;
            a node still in-degree>0 is referenced again and was never reused-as-condemned anyway. The
            retention backstop applies only to the reconcile scan, not to this fast EBR reclaim.)
```
- `e+2` = Crossbeam 3-epoch limbo; `safe_epoch > e_a` = QSBR/EBR quiescence; `Retention` = the Iceberg-style
  time backstop; `still-leader(fence)` gates every DELETE.
- The cascade is crash-safe: a partial cascade is recovered by re-folding the durable `-` edges; missing `-`s
  only over-count (safe). Successor leader re-derives candidates under its own fence.

## 7. Lease, fence, and recovery {#recovery}

- **Writer lease (the hinge).** The Keeper ephemeral session IS the lease; `writers/<S>` carries `O_W`.
  Self-fence = act only while `Connected ∧ local-deadline inside T_session` (TLC CE-4: `Disconnected`-detection
  alone is insufficient — the local deadline is load-bearing). No inter-clock-skew assumption (single session
  timeout).
- **Leader fence.** Lowest-seq ephemeral; `<seq>` is the fence; every mutation re-checks it with a `sync`-ed
  read; fail-close on `Disconnected`.
- **Total Keeper loss → safe pause → S3 recovery.**
  ```
  during:  every writer session drops → READ-ONLY; leader lock gone → GC stops.  No dangling mutation occurs.
  restore: PURGE any backup-restored ghost leader//writers/ znodes; elect a leader;
           E := GET gc/epoch (S3); fenced PUT gc/epoch = E+1 (fence off pre-outage in-flight epochs);
           writers reconnect, re-create writers/<S>, observe the new epoch, resume.
           If snap is in doubt → RECONCILE: full DAG reachability from refs/ (the durable, written-last
           authority) rebuilds snap; reconcile never reclaims anything younger than Retention.  S3 alone suffices.
  ```

## 8. What is verified vs open {#status}

TLC-checked (bounded) for the *single-node* core: `INV_NO_LOSS / NO_DANGLE / NO_ABA` under expiry-gap +
split-brain + Keeper-wipe. **Not yet modeled:** multi-child commit atomicity (a tree making a SET of nodes
reachable at once) and the decrement cascade — the #1 next model-checking target. **Decided** (see
`2026-06-07-ca-design-decisions.md`): Keeper required (D1), generations decoupled per-node (D2, resolves
CE-2), flat refs (D3), deferred cascade (D4), hashes = `cityHash128`/`SipHash-128`. Still open: the folder API
(`putBlob/putTree/getBlob/getTree/setRef/removeRef`) and the `IDataPartStorage` adapter.
