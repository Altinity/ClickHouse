# Content-addressed storage as a Merkle DAG of immutable folders (MergeTree parts as a corner case)

The unifying foundation for the content-addressed (CA) backend: the store is a **Merkle DAG of immutable
folders** — blobs (files) + trees (folders) + mutable refs — i.e. Git's object model. A MergeTree **part is
just a tree**; projections are subtrees; mutations are structural sharing. The Keeper-only EBR garbage
collector (`2026-06-07-ca-gc-keeper-only-profile.md`) operates on this DAG with "blob" generalized to "node"
and "reference" generalized to a DAG **edge**.

## 1. Object model {#model}

Three things; everything else is built from them:

- **Blob** — opaque immutable bytes (one file's content). Key `blobs/<H>` where `H = hash(bytes)`.
- **Tree** — an immutable folder: a **canonically serialized**, name-sorted list of entries
  `(name, kind ∈ {blob,tree}, child_hash)`. Key `trees/<T>` where `T = hash(serialized entries)`. A tree's
  hash therefore commits to every descendant — this is the Merkle property.
- **Ref** — the only mutable object: a named pointer `name → node_hash` (usually a tree). Refs are the GC
  roots and the commit points (written last; see the GC profile).

The store is thus a **Merkle DAG**, and it is **acyclic by construction**: computing a parent's hash requires
its children's hashes already exist, so no node can (transitively) point at itself. Consequence:
**per-node reference counting is *complete*** — `in-degree == 0` ⇒ genuinely unreachable (no cycle can keep a
dead island alive), so we never need mark-sweep to break cycles.

### Canonical serialization {#canonical}
Identical folders must hash identically, so the tree encoding is fixed: entries sorted by `name`, a fixed
field layout, explicit `kind`, no timestamps/uids/ordering nondeterminism (Git's tree format is the proven
template). This is what makes recursive dedup work — same content, same hash, everywhere.

## 2. MergeTree as a corner case {#mergetree}

| MergeTree concept | Merkle-folder representation |
|---|---|
| part file (`.bin`, `.mrk`, `primary.idx`, `columns.txt`, …) | **blob** |
| a part | **tree** of its files; `part_id` ≡ the part-tree's hash `T` (this is today's `PartManifest`, reframed as a general tree) |
| projections / nested per-part dirs | **subtrees** (a tree entry whose `kind = tree`) — recursive, no special case |
| table active set | **refs**, one per part name → part-tree hash (kept flat; see §5 scale) |
| `MutateTask` carry-forward / merge of unchanged columns | a **new part-tree reusing the unchanged children's hashes** — structural sharing; unchanged blobs/subtrees are not re-uploaded |
| dedup | **recursive and automatic**: equal file → same blob; equal folder → same tree; equal part → same tree |
| FREEZE / shadow, BACKUP | a **ref** (or a tree) pinning a part-tree — same machinery, table-lifetime-independent |

The `PartManifest` special-case disappears: it is a tree. Every folder-shaped artifact (parts, projections,
backups, and future features) goes through **one immutable-folder API** (`putTree`, `getTree`, `putBlob`,
`getBlob`, `setRef`, `removeRef`). `IDataPartStorage` for this metadata type becomes a thin adapter that maps a
part directory ⇄ a tree.

## 3. Writing a part (structural sharing) {#write}

```
build the part locally (unchanged: merge/insert runs on local scratch)
for each file f:        Bf := hash(bytes(f));  putBlob(Bf)             (content-addressed, dedup)
for each subdir d:      Td := putTree(entries(d))                       (recurse; subtree dedup)
T := putTree( sorted [(name, kind, child_hash) for each file/subdir] ) (the part-tree; part_id == T)
setRef(store/<srv>/<uuid>/refs/<part_name>, T)                          (commit point, written LAST)
```
A mutation that changes one column rewrites only that blob and the part-tree (new `T'`); every other entry in
`T'` reuses the existing child hash — no copy. Identical content across parts/merges collapses to the same
blob/tree by hash. (This is exactly a Git commit's tree-sharing.)

## 4. GC over the DAG {#gc}

The Keeper-only EBR GC generalizes with two substitutions — **node** = blob or tree; **edge** = a reference,
either `ref → node` (root edge) or `tree → child` (internal edge):

- **Roots** = live refs (+ in-flight write leases, + shadow/frozen refs), as before.
- **Liveness** = reachable from a root through tree edges. Because the DAG is acyclic, **`in-degree == 0`
  (no ref, no parent tree) is a complete deadness test** — refcounting suffices.
- **The `+`/`-` log records EDGES.** Committing a part-tree appends `+` for `ref→T` and for each `T→child`;
  dropping a part appends `-` for `ref→T`, and when a node's in-degree reaches 0 it is condemned and reclaiming
  it appends `-` for each of *its* outgoing edges — a **decrement cascade** down the dead subtree.
- **Everything else is unchanged:** the sharded snapshot folds per-node in-degree (the sloppy candidate
  filter); the fenced single leader condemns (`<gen>.tombstone`) and reclaims under the `e+2` limbo +
  `safe_epoch` quiescence; epoch (EBR clock) + a decoupled per-node resurrection generation (D2); per-writer
  time-bounded lease (session-alive self-fence);
  retention-backstop reconcile = full **DAG reachability** from refs, the rebuild authority.
- **Cascade crash-safety:** a partial decrement cascade (leader crash mid-cascade) is recovered by re-folding
  the log — the `-` edges already written are durable, and a re-fold recomputes in-degrees; missing `-`s only
  *over-count* (a node kept one round longer), which is safe (`INV-OVER-COUNT-ONLY`). The successor leader
  re-folds + re-quiesces under its own fence before deleting (as in the profile).
- **CE-2 generalizes to nodes:** dedup-reusing an existing subtree races a concurrent condemn exactly like a
  blob; the same reuse rule applies uniformly — *reuse → make the edge-`+` durable → re-check the tombstone →
  resurrect (re-create at gen+1) only if now condemned*. One rule for blobs and trees.

So the GC we model-checked (`docs/superpowers/models/`) applies; the model's `H`/`(H,e)` becomes a node, and
its single edge generalizes to the DAG edge set. (Extending the TLA+ model to a *tree referencing a set of
nodes* — multi-child commit atomicity — is the top untested gap, §6.)

## 5. Scale {#scale}

- **Parts are small trees** (tens–hundreds of files) — cheap to put/get/dedup.
- **Do NOT make the table one giant tree.** A table with millions of parts keeps **one ref per part**
  (the mutable layer), not a single multi-million-entry tree object. If a single huge directory is ever
  needed, shard it into a **trie of trees** by name-prefix (so a change touches O(log) trees, not a full
  rewrite) — but the default is flat refs.
- **High in-degree is expected and good** — an unchanged column blob shared across 1000 mutations has
  in-degree 1000; that *is* dedup. The refcount is a count, not a set; the log coalesces edge deltas, so write
  amplification stays ~O(distinct new edges per commit), comparable to today's per-file manifest pins.
- Reachability/`reconcile` is now a DAG walk (deeper than ref→manifest→blob), but it is off-hot-path; the hot
  path is the per-node in-degree candidate filter (sharded fold = O(delta)).

## 6. Why this is the right foundation {#why}

- **Uniform** — one immutable-folder abstraction; parts, projections, nesting, backups stop being special.
- **Recursive dedup + structural sharing** — for free, at every level, by hash. Mutations/merges become
  cheap tree rewrites.
- **GC is simpler to reason about** — acyclic Merkle DAG ⇒ refcounting is complete (no cycle collector), and
  the EBR machinery we already hardened + model-checked applies node-for-node.
- **Battle-proven shape** — Git (blob/tree/commit/ref) and IPFS (Merkle DAG) are exactly this; we inherit
  decades of validation of the data model (not the GC — that's our EBR layer).

## 7. Open questions {#open}

1. **Commit atomicity of a multi-node tree** — publishing a part-tree makes a *set* of nodes reachable at once
   (the tree + all its new blobs/subtrees). The write order (children before parent before ref) + the edge-`+`s
   must keep `INV-NO-LOSS` for the whole set. **This is the #1 thing to add to the TLA+ model** (currently a
   single `H`; needs a node referencing a *set* of children).
2. **CE-2 reuse rule for trees** — confirm `reuse + durable edge-`+` + re-check-tombstone` resurrects correctly
   when a *subtree* (not just a blob) is condemned mid-reuse. (The `generation == epoch` vs long-lived-dedup
   tension is RESOLVED — D2 decouples generation into a per-node resurrection counter; see
   `2026-06-07-ca-design-decisions.md`.)
3. **Tree size / sharding policy** — define when a tree must be sharded into a trie (large-directory corner),
   and the canonical serialization + hash function (collision resistance, length).
4. **API surface** — the minimal `putBlob/putTree/getBlob/getTree/setRef/removeRef` interface and how
   `IDataPartStorage` adapts onto it; what the reader path looks like (resolve ref → tree → walk).
5. **Migration** — the current implementation already has blobs + manifests + refs; generalizing the manifest
   into a recursive tree is an incremental step, not a rewrite.

## 8. Status {#status}

Proposed new foundation; supersedes the part-specific framing of the prior docs as the *data model*, while
**reusing** the Keeper-only EBR GC (`2026-06-07-ca-gc-keeper-only-profile.md`) as the *reclamation layer*
(generalized blob→node, reference→edge). Not yet reviewed or model-checked at the tree level — §6/§7 are next.
