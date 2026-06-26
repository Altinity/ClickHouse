---
description: Design for a streaming GC based on root-local full-tree manifests and content-addressed blobs
sidebar_label: CAS GC full-tree manifest redesign
sidebar_position: 1
slug: /superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design
title: CA GC root-local full-tree manifest redesign design
doc_type: reference
---

# CA GC - root-local full-tree manifest redesign - design {#ca-gc-root-local-full-tree-manifest-redesign-design}

**Status:** design (2026-06-26, rev. 8). Branch `codex-gc-proposal-2026-06-26`.
**NOT behavior-preserving.** This is an architectural redesign of CA tree identity, build precommit,
read-path tree resolution, and GC accounting. CA is pre-release, so no persisted-data compatibility
path is required. Every behavior-changing phase is gated on a green `TLA+` model extension before code
lands.

The core change is now sharper than rev. 7: **only blobs remain content-addressed**. A root-local
immutable tree object is not a folder node and does not point to child tree objects. It is a
**full-tree manifest**: one immutable object containing the complete logical subtree, including all
file paths, inline payloads, and blob references. Directories inside it are path prefixes or an
optional internal index, not protocol objects.

This removes the last recursive tree walk from the GC protocol. A root/precommit owner transition
names one full-tree manifest id. GC reads that one manifest object as a stream and emits blob deltas.
There are no nested tree instance ids, no parent-child tree ownership, no tree expansion markers, no
GC-side cascade state, and no full-closure root journal records.

## Goals {#goals}

The design must satisfy the original requirements:

- **R0: safety is non-negotiable and `TLA+`-provable.** No delay, race, duplicate worker, interrupted
  attempt, stale view, or backend reorder may make the system less reliable. `INV_NO_DANGLE`,
  `INV_NO_LOSS`, and `INV_NO_RETURN` must be proved, not argued.
- **R1: each GC round is optimal.** Work is proportional to changed root owner transitions,
  deactivated full-tree manifests, and blobs whose in-degree changes. Memory is bounded by stream
  buffers. GC state is stored as coarse write-once objects.
- **R2: target-shardable GC.** Default `gc_shards = 1`, with the option for different replicas to own
  disjoint blob target shards.
- **R3: simple, reliable, debuggable.** Durable state must explain what every round has folded,
  retired, fenced, rechecked, deleted, and trimmed. Every step must be resumable and idempotent.

Secondary design goals:

- remove root-ref full-closure journal payloads;
- remove content-addressed tree revival as a class of races;
- remove nested static-folder protocol objects;
- remove `TreeExpansionIndex`, `children_by_tree`, expansion markers, and cascade-wave state from GC;
- keep exact-token delete and the global fence/recheck proof shape;
- move precommit/build state into the target root prefix and make promotion an owner move.

## Protocol Boundary {#protocol-boundary}

This is not a local GC optimization. It changes the formal object model, the root journal contract,
the read path, and the meaning of `fold`.

The `TLA+` model must change because the current model treats trees as content hashes with `treeEdges`,
`marker`, and a cascade action. Rev. 8 needs a new model branch with:

- random 128-bit `tree_instance_id` values for full-tree manifests;
- root-local immutable full-tree manifest bodies;
- no nested tree objects and no child tree edges;
- `SingleManifestOwner` and `NoTreeIdReuse`;
- precommit owners, committed owners, and atomic `PromotePrecommit`;
- precommit-visible missing blob leaves;
- blob in-degree derived only from active full-tree manifests;
- manifest-delete work keyed by `tree_instance_id`;
- namespace-scoped orphan manifest debris for objects written before `PrecommitAdd`;
- the existing global registry/root fence and fold-through-fence recheck.

The visible round skeleton intentionally stays close to the proved tail:

```
discover -> fold -> retire -> fence -> recheck -> exact-token delete -> trim
```

What changes is `fold`. Today it folds root ref records into root edges, expands content-addressed
trees, and later relies on cascade to remove child edges. Rev. 8 folds root owner transitions, reads a
single root-local full-tree manifest object for each old/new owner, and emits blob deltas directly.
There is no resident tree-child authority inside GC after the fold output is sealed.

The following principles are unchanged and remain load-bearing:

- exact-token delete;
- global registry fence before root-shard fences;
- fold-through-fence recheck before delete;
- `ViewableRound` barrier before writers can observe retired tokens;
- `deadTok` / no-return for blob tokens;
- fail-closed publish and promotion gates;
- registry as authority, with LIST only as an accelerator.

## Ground Truth In The Current Code {#ground-truth-in-the-current-code}

The current implementation has four load-bearing properties that the new design must preserve:

- `CasGc.cpp` runs `fold -> retire -> fence -> recheck -> exact-token delete -> cascade -> trim`.
  The lease is only work-dedup; the `TLA+` model proves safety without assuming a unique leader.
- `Gc::fold` hard-pins `snap_shards = 1` because last-op-wins displacement is currently folded inside
  one resident `GcSnap`. A target-sharded reducer cannot infer the old target of a root ref from the
  new target alone.
- `Gc::cascadeAndPersist` is safe today only because it removes child edges in the same round that
  confirms the tree deletion, before the cursor advances. Deferring a content-hash-keyed child-edge
  removal is the modeled `SabotageCascadeRace`: a later content-identical tree can reuse the same tree
  hash.
- `Build::precommit` currently uses a separate `<server_hex>/_precommits` namespace and populates
  `JournalRecord.closure` with `ClosureNodeProto` records for staged trees. This was the B199-S2 fix:
  GC must not have to read a vanished staged tree to learn the precommit closure.

The current code also shows where rev. 8 cuts complexity:

- `Build::stageTree` mints `TreeId` from a Merkle/content hash and retains encoded tree payload for
  re-upload. Rev. 8 changes `TreeId` to a root-local full-tree manifest id; the optional payload digest
  is only integrity/debug data.
- `Store::readTree` assumes trees are global immutable content objects under `trees/<prefix>/<hash>`.
  Rev. 8 reads full-tree manifests from the owning root prefix instead.
- `RootShardManifest` is a whole-object protobuf with `refs` and repeated `JournalRecord`. Rev. 8
  still wants the planned streaming protobuf migration, but the records are compact owner transitions,
  not full closures.
- `CasLayout` has global `blobKey` and `treeKey`. Rev. 8 keeps `blobKey` and replaces CA `treeKey`
  with root-local full-tree manifest keys.

## Formal Model Ground Truth {#formal-model-ground-truth}

`CaIncarnationCore.tla` proves the core delete protocol:

- `GRetire` records the exact current token of an in-degree-zero object.
- `GFenceRegistry` precedes root-shard fences and closes namespace-creation races.
- `GRecheckDelete` requires fold-through-fence before issuing the exact-token delete.
- `ViewableRound` says a writer may observe round `R` only after all round-`R` retired sets are
  durable.
- `deadTok` prevents a deleted or overwritten token from becoming a valid dependency again.

`CaBuildRootPrecommit.tla` proves the build-root/precommit fix:

- a live precommit protects a build's staged structure;
- a committed table ref is allowed only after a fail-closed full-closure presence recheck;
- precommit may reference missing blobs, but a committed root may not;
- reclaiming a precommit of a still-live but judged-dead build is safe only because `Commit` rechecks
  all blob leaves before publishing.

Rev. 8 keeps these proof obligations, but changes the tree model:

- tree identities are no longer content hashes;
- a full-tree manifest is a single-owner root-local object;
- there is no tree-child protocol graph;
- only blob objects are content-addressed, deduplicated, and shared across roots;
- a full-tree manifest id is never reused after it becomes visible in any root journal.

## Core Principle {#core-principle}

There are only two protocol-level folder concepts:

- **Dynamic folders** are root manifests. They have a mutable journal and own top-level full-tree
  manifests through owner transitions.
- **Static folders** are full-tree manifest objects. They are immutable and indivisible. Their body is
  the complete subtree: every file path, every inline value, and every blob reference under that root.

The reference graph is therefore:

```
dynamic root/precommit ref -> root-local full-tree manifest
full-tree manifest         -> content-addressed blob
```

Directories inside a full-tree manifest are not separate objects. They are represented by path
prefixes. If lookup/list performance needs it, the same manifest object may include an internal
directory index, but that index is still part of the single immutable manifest payload.

Only blobs are content-addressed. Full-tree manifests are not deduplicated by payload. If two parts
have byte-identical logical metadata, they still get different manifest ids. The payload may carry a
digest for corruption detection, but that digest is not identity and is never used as a GC key.

This is the key simplification: stale work is keyed by a unique full-tree manifest id, not by a content
hash that a future publish can revive.

## Object Identity And Ownership {#object-identity-and-ownership}

### Blob Identity {#blob-identity}

Blob identity remains unchanged:

```
blob_hash = hash(blob_payload)
key       = <pool>/blobs/<first2>/<blob_hash>
```

Blob payloads keep incarnation tokens. Exact-token delete remains the only destructive content delete
authority. Blob dedup remains the main storage win.

### Full-Tree Manifest Identity {#full-tree-manifest-identity}

A full-tree manifest id is a random 128-bit value minted by the writer and unique within the owning
root namespace:

```
tree_instance_id = random_u128
```

Writer id, build sequence, and build-local ordinal are debug/body fields, not safety identity. The
requirements are:

- no manifest id is derived from payload;
- no visible manifest id is ever reused;
- manifest objects are immutable write-once objects;
- an id collision fails closed before any root journal transition becomes visible.

The manifest body may include:

```
FullTreeManifestProto {
    header
    root_namespace_id
    tree_instance_id
    writer_id
    build_seq
    build_ordinal
    payload_digest
    repeated Entry entries
    optional DirectoryIndex directory_index
}

Entry {
    path
    placement = inline | blob
    blob_hash
    blob_size
    inline_bytes
}
```

Entries are canonicalized by path. Duplicate paths are corruption. Duplicate blob hashes across
different paths are allowed and must be folded by deterministic source edge ids such as
`(tree_instance_id, path)`.

`payload_digest` is an integrity check and a debug aid. It is not a storage key, not a dedup key, and
not an in-degree key.

### Manifest Ownership {#manifest-ownership}

A visible full-tree manifest has at most one structural owner:

- a committed root ref;
- a precommit ref.

There is no parent-tree owner because there are no nested tree objects. Promotion moves ownership from
a precommit ref to a committed root ref. It must be one root-shard CAS transition, not "add table ref
now, best-effort remove precommit later". The current two-namespace best-effort precommit removal is
safe in the old model because duplicate edges only over-protect; this design chooses the simpler
invariant instead: a manifest owner is singular and explicit.

An implementation may represent the move as `old_owner -> new_owner` inside one journal record. It
must not expose a state where neither owner exists.

## S3 Layout {#s3-layout}

Blobs stay global:

```
<pool>/blobs/<aa>/<blob_hash>
```

Root-local full-tree manifests live under the owning root prefix:

```
<pool>/roots/<root_namespace>/
  <root_shard_number>
  _files/...
  _subtrees/<writer_id>/<build_seq>/<aa>/<tree_instance_id>.proto
```

`_subtrees` becomes a reserved segment, like `_files`. The name is kept because it describes the logical
object: one immutable subtree manifest. It does not imply nested protocol objects.

The `<writer_id>/<build_seq>` prefix is for cleanup and diagnostics only; `tree_instance_id` remains
the random 128-bit safety identity. The `<aa>` fanout is derived from `tree_instance_id`, not from the
payload digest.

For the user's example namespace, the layout can be:

```
server/store/ab/uuid@cas@/
  17
  _files/...
  _subtrees/srv-a/1042/7f/7f3a...c1.proto
```

where `17` is a root shard manifest and `_subtrees/srv-a/1042/7f/7f3a...c1.proto` is a root-local
full-tree manifest staged by build sequence `1042`.

Important: the build-scoped prefix is not a deletion authority. A manifest can remain live after the
build sequence is below the watermark because promotion keeps the same manifest id and object. Cleanup
must check liveness per manifest id against a sealed namespace owner view; it must never delete a whole
build prefix blindly.

## Root Journal Format {#root-journal-format}

The root journal no longer carries transitive closures. It carries compact owner transitions:

```
OwnerTransition {
    transition_version
    ref_name
    old_tree_instance_id?   // owner removed
    new_tree_instance_id?   // owner added
}

PrecommitTransition {
    transition_version
    build_id
    final_ref_name
    old_tree_instance_id?
    new_tree_instance_id?
}

PromotePrecommit {
    transition_version
    build_id
    final_ref_name
    tree_instance_id
}
```

Semantics:

- create precommit: `old = none`, `new = T`;
- abandon/reclaim precommit: `old = T`, `new = none`;
- publish new ref without precommit: `old = none`, `new = T`;
- drop ref: `old = T`, `new = none`;
- repoint ref: `old = T_old`, `new = T_new`;
- promote precommit: owner moves from `precommit(build_id)` to `ref(final_ref_name)` with no blob-edge
  change.

The current `JournalRecord{Add, Remove}` shape is insufficient for target-sharded streaming because
the target reducer cannot infer the displaced old tree. The new journal must encode replace/move
semantics at the root source. With that, rev. 5's durable `RootEdgeIndex` is no longer a required
long-lived GC object: root-shard mappers can emit paired old/new deltas from one source transition.

The planned streaming protobuf migration still matters. A root manifest should be readable as a
length-delimited record stream so GC can fold journal records without materializing the whole root
manifest in memory:

```
ManifestHeader
RefRecord...
PrecommitRecord...
OwnerTransitionRecord...
ManifestFooter
```

## Read Path Scope {#read-path-scope}

This redesign changes the query-hot read path.

`Store::resolveRef` resolves a ref to a root-local `tree_instance_id` plus its owning root namespace.
`Store::readTree` no longer reads `trees/<hash>`. It reads the full-tree manifest from the root prefix,
then answers path lookup or directory listing from the manifest entries and optional directory index.

The old per-process tree decode cache shared by content hash becomes less useful because each publish
uses a unique manifest id. That is an intentional tradeoff. The data that matters for storage and
large reads is still blob-deduplicated; tree metadata is small, immutable, and can be cached by
`(root_namespace, tree_instance_id, token)`.

Planning must include:

- path lookup over a full manifest;
- optional directory index for fast `listDirectory`-style operations;
- bounded decode/cache memory;
- fail-closed behavior when a committed ref names a missing manifest.

## Build And Precommit Protocol {#build-and-precommit-protocol}

Precommit should live in the target root prefix and follow the same ownership rules as committed refs.
The only semantic relaxation is: **precommit may reference missing blobs; committed refs may not**.

### Stage Full-Tree Manifest {#stage-full-tree-manifest}

`Build::stageTree` changes from "compute Merkle `TreeId`" to "mint root-local full-tree manifest id".

The writer builds one full-tree manifest:

1. Resolve the complete logical subtree into canonical entries: full path, placement, blob hash/size or
   inline bytes.
2. Mint one random 128-bit `tree_instance_id`.
3. Write the manifest object under `_subtrees/<writer_id>/<build_seq>/...`.
4. Keep the encoded manifest payload in memory until the precommit root transition is durable.

The manifest object named by a visible precommit must already exist. Blobs named by that manifest may
still be absent.

This is the replacement for `JournalRecord.closure`: the full-tree manifest itself is the durable
staged structure. GC never needs a full closure copied into the root journal, and it never needs
recursive reads to reconstruct a staged precommit.

### Pre-Precommit Manifest Debris {#pre-precommit-manifest-debris}

Manifest bodies are written before `PrecommitAdd`. If a build stops in that window, no owner
transition names those manifest ids. Owner-driven GC would never read or delete them unless the
protocol adds an explicit debris path.

This section defines the build-side contract. The round-level cleanup algorithm is specified in
[Orphan Manifest-Debris Sweep](#orphan-manifest-debris-sweep).

Rev. 8 makes this a first-class liveness obligation:

- writer abort does best-effort deletion of its own `_subtrees/<writer_id>/<build_seq>/` objects;
- GC has a rare backstop sweep for build-scoped `_subtrees` objects whose build is below the server
  watermark floor, retired, or judged dead by the same rules used for precommit reclaim;
- the sweep is scoped to one root namespace at a time;
- the sweep builds the active manifest-id set from that namespace's sealed root/precommit owner view;
- the sweep deletes only dead-build manifest objects whose ids are absent from that namespace active
  set;
- every manifest delete is exact-token;
- if the build was falsely judged dead and later tries to publish `PrecommitAdd`, it must first
  re-check that the named manifest body still exists. A missing manifest makes `PrecommitAdd` fail
  closed and the writer retries with a fresh manifest id.

This is a space-liveness mechanism, not reader-facing correctness. A pre-precommit manifest has no root
owner, so readers cannot reach it. The important rule is that the debris sweep must never treat
"dead build prefix" as enough to delete. It needs a sealed per-namespace owner view and a per-object
liveness check.

The common case should be writer cleanup. The GC backstop is for stopped writers and must be bounded:
one namespace, one dead build prefix, and a limited number of exact-token deletes per round.

### Final Ref Name Requirement {#final-ref-name-requirement}

The normal precommit path requires `final_ref_name` up front. In `MergeTree`, the output part name is
known before the build: insert assigns it and merge/mutation knows its target. That lets
`PrecommitAdd` live in the same root shard as the final committed ref, so `PromotePrecommit` is one
root-shard CAS owner move.

Rev. 8 deliberately does not include a GC-visible scratch-root fallback. A caller that cannot know
`final_ref_name` must delay `PrecommitAdd` until the final name is known or get a separate modeled
design. A hidden scratch precommit would double-write manifests and add another debris location to
prove.

### Precommit Add {#precommit-add}

The precommit CAS is written to the same root namespace as the future committed ref. For a normal part
publish, the precommit is placed in the same root shard as `final_ref_name`, so promotion can be atomic
in one shard CAS.

The precommit record:

- names `build_id`;
- names `final_ref_name`;
- names the full-tree manifest id;
- records enough build watermark identity to let GC reclaim abandoned precommits;
- does not carry transitive closure.

After `PrecommitAdd` is visible, GC treats the manifest as active. Its blob edges protect all currently
present blobs in the manifest and also protect future uploads of the same blob hashes because the edge
already exists in the target in-degree state.

### Blob Uploads Under Precommit {#blob-uploads-under-precommit}

The modeled order is:

1. write the root-local full-tree manifest body;
2. publish `PrecommitAdd`;
3. upload or revalidate blob bodies;
4. promote only after a fail-closed blob recheck.

This order is possible because the manifest needs blob hashes and sizes, not necessarily blob bodies.
It gives the blob upload path a structural precommit edge before a newly uploaded blob can become an
in-degree-zero GC candidate.

Uploading a blob before `PrecommitAdd` is allowed only as unprotected speculative debris. GC may delete
it before the precommit appears. The writer must tolerate that by re-uploading from source or aborting
at promotion. The system must never treat a precommitless upload as protected.

The key invariant is:

- precommit manifest body must exist before precommit is visible;
- blob bodies may be missing while the owner is precommit;
- a blob that is present and reachable from a live precommit must not be deleted;
- a missing blob under precommit is not corruption.

This matches the existing `CaBuildRootPrecommit.tla` split between build-root intent and committed
reader-facing truth.

### Promote Precommit {#promote-precommit}

Commit promotion is a single root-shard CAS:

1. Refresh retire view if the root shard or registry fence demands it.
2. Revalidate every blob leaf listed in the precommit manifest.
3. If any blob is absent or condemned and not recreatable from writer source, fail closed with
   `ABORTED`.
4. Atomically replace `precommit(build_id)` owner with `ref(final_ref_name)` owner.
5. Append `PromotePrecommit` to the root journal.

No blob-edge delta is emitted for promotion because the same manifest remains active. The dynamic owner
changes, not the static manifest.

This is simpler than the current `Build::publish` plus best-effort precommit `Remove`: there is no
leftover stale precommit edge after a successful commit, and no second namespace that GC must discover,
fence, and reclaim.

Promotion revalidates the whole manifest. That is preexisting safety logic from the build-root model,
not a new weakness. It is `O(manifest entries)`; if it becomes visible in profiles, the optimization is
an internal per-manifest blob summary or directory/blob index, still inside the same immutable manifest
object.

### Abandon Or Reclaim Precommit {#abandon-or-reclaim-precommit}

An abandoned precommit owner is removed by GC or by writer cleanup:

```
PrecommitRemove(build_id, tree_instance_id)
```

The manifest id is unique and never reused. Therefore removing this owner later is safe even if a
different build later creates a byte-identical manifest under a different id.

If GC falsely reclaims a still-live build's precommit, the later `PromotePrecommit` must fail closed
because `precommit(build_id)` is no longer present. The writer retries by creating a fresh precommit
with a fresh manifest id, then revalidates blobs again.

## GC Authority Model {#gc-authority-model}

GC tracks blob reachability from active full-tree manifests:

```
blob_indeg[b] =
    Cardinality({
        source edge (tree_instance_id, path)
        : tree_instance_id is active
          and its entry at path references blob b
    })
```

Full-tree manifests are not target-counted like blobs. Their liveness is structural:

- a manifest is live iff it has a committed ref or precommit owner;
- if an owner is removed and not restored before the fence/recheck cut, the manifest is unreachable
  and can be deleted with an exact-token delete after its blob decrements are sealed.

This deletes an entire class of GC state:

- no target in-degree entries for tree objects;
- no child tree instances;
- no expansion markers;
- no `children_by_tree`;
- no durable `TreeExpansionIndex`;
- no same-round cascade wave;
- no cross-owner cascade barrier;
- no content-hash tree revival race.

GC still maintains a streaming blob in-degree generation. It may also maintain a coarse manifest-delete
work bundle for manifest ids whose owner was removed. That bundle is keyed by manifest id and round,
not by content hash, and is safe to replay.

## Round Protocol {#round-protocol}

The round remains recognizable:

```
discover -> fold owner transitions -> retire -> fence -> recheck -> exact-token delete -> trim
```

There is no GC-side cascade step.

"No cascade" here means no deferred child-edge state and no revival-prone tree-hash-keyed child-edge
removal. It does **not** mean the ordering obligation disappears. For an owner removal, GC must read
the full-tree manifest while it is still available, seal the corresponding blob decrements into the
generation, and only then allow exact-token deletion of the manifest body. The old cascade ordering
becomes a simpler revival-proof ordering on unique manifest ids.

### Discovery {#discovery}

`gc/registry` remains the authority for the namespace universe. LIST is only an accelerator:

- listed root token equals persisted folded token: skip body read;
- token missing, ambiguous, stale, or unsupported: read the root shard body;
- registry namespace missing from LIST: read/mint according to current registry rules;
- LIST never shrinks the registry universe.

The token-diff optimization from rev. 5 still applies, but it is now even smaller: changed root shards
contain compact owner transitions, not full closures.

### Fold Owner Transitions {#fold-owner-transitions}

For every changed root shard, stream owner transitions in root-journal order.

For `old_tree_instance_id`:

- read the one root-local full-tree manifest object;
- stream its entries once;
- emit `-1` blob-edge deltas for every blob entry;
- record the manifest id as delete work for this round/generation.

For `new_tree_instance_id`:

- verify the manifest object exists and belongs to this root namespace;
- stream its entries once;
- emit `+1` blob-edge deltas for every blob entry.

For `PromotePrecommit`:

- update owner metadata;
- emit no blob deltas.

The fold is streaming. It keeps only a manifest record buffer and target-sort/spill buffers. It does
not build a resident closure set and does not recursively read any child manifests. Duplicate blob
references are handled by deterministic source edge ids `(tree_instance_id, path)`.

A repoint/mutation over a large manifest emits `-old` and `+new` by reading both full manifests. Many
unchanged blob refs may cancel in the target reducer. The design deliberately avoids a tree-diff
protocol: manifests are metadata, writes stay compact, and the reducer handles cancellation in
background.

Fold output is sealed in a write-once generation manifest:

```
gc/gen/<generation>/manifest
gc/gen/<generation>/blob_target/<target_shard>/...
gc/gen/<generation>/manifest_delete_work/<owner_shard>/...
```

These are coarse files. There is no object per edge, manifest, or candidate.

### Retire {#retire}

Blob retire scans the touched blob target shards and emits candidates whose in-degree transitioned to
zero. It keeps the existing per-candidate `HEAD` until a separate model proves a token-source
optimization.

Manifest retire consumes manifest-delete work:

- read each manifest object token, or reuse the token captured during fold if the backend proof covers
  it;
- write a compact retired-manifest bundle;
- do not delete the manifest object until the fence/recheck phase confirms the owner removal.

Manifest delete is space cleanup, not reader-facing correctness. If a manifest delete is delayed, the
manifest is unreachable debris. It must not keep blob in-degree elevated after its owner removal has
been folded.

### Orphan Manifest-Debris Sweep {#orphan-manifest-debris-sweep}

The regular owner-transition fold cannot see manifest bodies written before `PrecommitAdd`. A bounded
background sweep handles those objects. This is the GC-side backstop for
[Pre-Precommit Manifest Debris](#pre-precommit-manifest-debris), not a second cleanup protocol.

The sweep is **per namespace**, not pool-wide:

- choose one root namespace and one dead build prefix `_subtrees/<writer_id>/<build_seq>/`;
- build the active manifest-id set from that namespace's sealed committed-ref and live-precommit owner
  view;
- enumerate that one build prefix;
- delete only manifest objects whose ids are absent from the active set, using exact tokens;
- record compact outcomes so the sweep can resume without depending on LIST stability.

This sweep is not allowed to emit blob decrements, because a pre-precommit manifest never contributed
blob increments. If a manifest id is found in the owner view, it leaves the sweep and is handled only by
the normal owner-transition path.

### Retire Visibility Barrier {#retire-visibility-barrier}

`gc/state.round` advances only after every blob target shard's retired set and every manifest-delete
bundle for the round are durable. This preserves `ViewableRound`: a writer refreshing to round `R` sees
the complete retired-token set, not a subset produced by faster reducers.

### Global Fence {#global-fence}

The fence remains global and coordinator-owned:

1. fence `gc/registry`;
2. fence every root shard in the fence universe, or a model-proven lazy subset;
3. record fence positions in the generation manifest.

The fence is still load-bearing. A publish into one root shard can protect blobs in any target shard,
so target reducers do not own independent fences.

### Recheck And Delete {#recheck-and-delete}

Recheck folds owner transitions through the durable fence positions into the completion generation.
Then:

- a blob candidate is deleted only if its blob in-degree is still zero and the exact token still
  matches;
- a manifest is deleted only if the manifest id still has no committed/precommit owner in the
  fold-through-fence view;
- every delete is exact-token;
- token mismatch is spared/replaced, not destructive;
- absent under a held retired token is an idempotent confirmed outcome.

For manifests, recheck must not need to read a deleted manifest body to compute blob decrements. The
blob decrements were already produced during fold from the manifest body while it was still required to
be present. If the process is interrupted after a manifest object delete but before metadata cleanup,
the manifest-delete bundle is enough to resume outcome handling.

### Trim {#trim}

Trim root journals only below the cursor coverage recorded in the sealed generation. Trim manifest
delete work only after:

- blob deltas from the owner removal are incorporated into the durable blob generation;
- manifest exact-token outcomes are durable or the work is explicitly carried forward;
- retired sets/outcomes are handled according to the existing drop-on-confirmed-outcome rule.

`INV_JOURNAL_COVERAGE` remains mandatory: a recovery from the parent generation must be able to replay
every transition not already incorporated into an adopted sealed generation.

## Sharding Model {#sharding-model}

Default `gc_shards = 1`.

For sharded mode:

- root-shard mappers stream owner transitions and scatter blob deltas by blob hash;
- blob target reducers own disjoint blob hash ranges;
- manifest-delete workers own disjoint manifest id ranges or root namespaces;
- one coordinator owns registry fence, input seal, round visibility, global fence, and generation
  pointer advance;
- leases are work-dedup only.

Because root owner transitions carry both old and new manifest ids, target reducers do not need a
durable `RootEdgeIndex` to solve displacement. The displacement decision is made at the source root
shard, then the old/new manifest streams scatter blob deltas to target reducers.

Two replicas may process different blob target shards and manifest-delete ranges concurrently. No
writer can observe mapper/reducer products until the generation manifest is sealed.

## What Becomes Simpler {#what-becomes-simpler}

This redesign removes or shrinks several previously hard pieces:

- **No content-addressed tree revival race.** Byte-identical future manifests get different
  `tree_instance_id` values, so stale work for old manifest `T1` cannot delete new manifest `T2` or
  apply `T1`'s blob decrements to `T2`.
- **No full-closure root journal.** Publish/drop records name old/new manifest ids only. The complete
  tree structure is stored once, in the full-tree manifest object.
- **No recursive GC tree reads.** Fold reads one manifest object per old/new owner transition.
- **No nested static-folder ownership.** Directories are path prefixes inside the manifest, not objects
  in the protocol graph.
- **No GC-side cascade state.** Owner removal directly produces blob decrements and manifest-delete
  work. There is no later content-hash-keyed child-edge removal to apply.
- **No `TreeExpansionIndex`.** The manifest body is the durable edge journal. There is no separate
  expansion marker whose atomicity must be proved.
- **No tree in-degree target shards.** Manifests are single-owner structural objects. Blob reducers
  count only blob edges.
- **No durable `RootEdgeIndex` prerequisite for sharding.** Replace/move owner transitions solve
  last-op-wins at the root source.
- **Precommit is not a second namespace protocol.** It lives in the target root prefix and promotes by
  owner move.
- **Wide-table journal amplification disappears.** Rev. 5 wrote transitive closure into every owner
  transition. Rev. 8 writes each full-tree manifest once and writes compact owner transitions
  thereafter.
- **Debugging is more physical.** A manifest key shows its root namespace and build prefix, and its
  body explains every blob edge. A blob in-degree entry can point back to `(root_namespace,
  tree_instance_id, path)`.

The explicit tradeoff is also clear: tree dedup is gone. This is intentional. Tree objects are metadata
relative to blobs, and the storage saved by deduplicating tree metadata was buying a disproportionate
amount of GC protocol complexity.

## Safety Invariants {#safety-invariants}

The new `TLA+` model should be a branch of `CaIncarnationCore.tla` plus the useful precommit rules from
`CaBuildRootPrecommit.tla`.

Mandatory invariants and liveness obligations:

- `NoTreeIdReuse`: once a manifest id appears in a visible root journal, no later action can bind that
  id to a different payload or owner lineage.
- `SingleManifestOwner`: each visible manifest has at most one structural owner. `PromotePrecommit` is
  an atomic owner move.
- `ManifestBodyBeforeVisible`: a root/precommit owner transition cannot become visible until the named
  manifest object exists.
- `CommittedNoMissingBlob`: every blob reachable from a committed root ref is present and not condemned
  in the committing writer's view.
- `PrecommitMayReferenceMissingBlob`: missing blob leaves under precommit are legal and not corruption.
- `BlobInDegreeMatchesActiveManifests`: durable blob target state equals the multiset of blob edges
  emitted by active manifests.
- `NoCommittedDangle`: a committed root ref never resolves to a missing manifest body or missing blob
  leaf.
- `NoReturn`: a deleted or overwritten blob token is never accepted as a future dependency.
- `ExactDeleteOnly`: destructive blob and manifest deletes require the exact observed token.
- `ViewableRound`: round `R` is writer-visible only after all retired sets and manifest-delete bundles
  for `R` are durable.
- `JournalCoverage`: no root transition or manifest-delete work is trimmed before its effect is
  included in a sealed generation or carried forward.
- `OrphanManifestDebrisDrains`: a manifest body staged before `PrecommitAdd` and never named by any
  live owner is eventually deleted after its build is abandoned or judged dead.

## Negative Controls {#negative-controls}

Each negative control must produce the expected counterexample before the phase is allowed to ship:

1. Reuse a manifest id for a byte-identical future manifest: stale manifest-delete work deletes the new
   manifest or applies old blob decrements to the new owner.
2. Allow two owners for one manifest id: deleting one owner removes blob edges still needed by the other
   owner.
3. Make `PromotePrecommit` two separate CAS operations with a gap and no fail-closed retry: a false
   precommit reclaim can create a committed dangle.
4. Let precommit become visible before the manifest body is durable: GC cannot emit the required blob
   edges and either leaks or under-protects.
5. Let committed publish skip blob revalidation: a committed ref can name an absent blob.
6. Treat a precommitless speculative blob upload as protected by the future build: GC may delete it
   before `PrecommitAdd`, so promotion must revalidate/reupload rather than assume protection.
7. Omit the pre-precommit `_subtrees` debris sweep: manifest bodies written before `PrecommitAdd` and
   then abandoned are never reachable from owner-transition fold and leak forever.
8. Delete build-scoped `_subtrees` debris using a partial owner view, or by wholesale deleting a dead
   build prefix: a live precommit or committed ref can lose its manifest body and become dangling.
9. Treat a missing manifest body under committed owner as an empty manifest: undercounts blob edges and
   permits over-delete.
10. Delete a manifest body before its owner-removal blob decrements are durable or carried forward:
   child blob references leak because the body needed for subtraction is gone.
11. Advance root cursor/token past unsealed owner-transition deltas: `SabotageCutOverclaim` shape.
12. Advance `gc/state.round` after only one target shard's retired set is durable: `ViewableRound`
   break.
13. Skip global fence for a racing publish: `INV_NO_DANGLE`.
14. Trim root journal below an unincorporated owner transition: `INV_JOURNAL_COVERAGE`.
15. Use non-exact delete or reuse blob tokens: existing `SabotageUncondDelete` and `SabotageReusedTag`
   shapes.

## Backpressure And Journal Encoding {#backpressure-and-journal-encoding}

Backpressure remains necessary, but the pressure points are smaller:

- root journal unfolded bytes;
- root manifest encoded bytes;
- number of owner transitions since folded cursor;
- full-tree manifest objects staged per build;
- full-tree manifest bytes per owner transition;
- blob delta bytes per generation;
- active precommit count and oldest precommit age.

The streaming protobuf migration should proceed, but it no longer needs to carry huge closure blocks.
The root stream consists of small owner/move records plus refs/precommits. Full-tree manifest bodies are
separate root-local protobuf objects and can use the same record/framing conventions.

When limits are exceeded, fail closed before publishing a root/precommit owner transition. Never publish
an owner transition that names a missing manifest body. Never publish a committed owner transition whose
blob leaves did not pass revalidation.

## Phase Plan {#phase-plan}

### Phase 0 - Model And Format Skeleton {#phase-0-model-and-format-skeleton}

- Create `CaGcRootLocalFullTreeCore.tla`.
- Model unique manifest ids, single ownership, precommit owner, committed owner, owner moves, missing
  blobs under precommit, fail-closed promotion, blob target in-degree, global fence, recheck,
  exact-token delete, manifest-delete work, and pre-precommit orphan manifest debris.
- Add negative controls listed above.
- Define `FullTreeManifestProto`, owner-transition records, and streaming root manifest framing.
- Define reserved `_subtrees` layout and key validation.

No production behavior changes in this phase.

### Phase 1 - Full-Tree Manifests, Single GC Shard {#phase-1-full-tree-manifests-single-gc-shard}

- Change `Build::stageTree` to mint root-local full-tree manifest ids instead of Merkle `TreeId`
  values.
- Write full-tree manifest objects under `_subtrees`.
- Add writer best-effort cleanup and GC backstop sweep for pre-precommit `_subtrees` debris.
- Move precommit into the target root prefix.
- Add atomic `PromotePrecommit`.
- Replace `JournalRecord{Add, Remove}` reachability records with owner transitions carrying old/new
  manifest ids.
- Build a streaming blob in-degree generation from owner-transition manifest streams.
- Update `Store::resolveRef`, `Store::readTree`, and metadata read helpers to read root-local full-tree
  manifests and optional internal directory indexes.
- Keep `gc_shards = 1`.
- Keep all-shard root scan and all-shard fence for the first behavior-changing implementation.
- Keep per-candidate `HEAD` in retire.

This phase removes global content-addressed tree objects, inline precommit closure records, resident
whole-pool `GcSnap` authority, expansion markers, nested tree objects, and cascade state.

### Phase 2 - Token-Diff Discovery And Lazy Read {#phase-2-token-diff-discovery-and-lazy-read}

- Add LIST token capability probes.
- Persist folded root manifest tokens with folded cursors.
- Skip unchanged root shards only when LIST token freshness is proved.
- Keep fail-closed fallback to body reads on ambiguous/missing tokens.

### Phase 3 - Lazy Fence And Lazy Trim {#phase-3-lazy-fence-and-lazy-trim}

- Add model-proven lazy fence rules.
- New/ambiguous/changed shards are fenced.
- Skipped shards may reuse a previous fence only when the model proves both no-dangle horns remain
  closed.
- Trim only below sealed generation coverage.

### Phase 4 - Target-Sharded Blob Reducers {#phase-4-target-sharded-blob-reducers}

- Enable `gc_shards > 1`.
- Root mappers scatter blob deltas by blob hash.
- Blob reducers merge disjoint target shards.
- Manifest-delete workers process disjoint manifest id ranges or namespaces.
- The global coordinator keeps the registry/fence/round-visibility responsibilities.

### Phase 5 - Retire-Token Optimization {#phase-5-retire-token-optimization}

Optional and separate. Remove per-candidate `HEAD` only after a model proves the stored token source is
complete and stale tokens can only cause under-delete, never over-delete.

## Operation Cost {#operation-cost}

| Quantity | Current code | Rev. 5 root-closure | Rev. 8 full-tree manifests |
|---|---|---|---|
| Tree identity | content hash | content hash + `ref_epoch` | unique root-local manifest id |
| Blob dedup | yes | yes | yes |
| Tree dedup | yes | yes | no |
| Publish journal size | tree hash | full transitive closure | old/new manifest ids |
| Tree structure storage | global `trees/` objects | global `trees/` object plus closure journal bytes | one root-local full-tree manifest |
| Read-path tree cache sharing | content-hash sharing | content-hash sharing | per-instance cache; less sharing |
| GC memory | resident `O(pool)` snap | stream buffers | stream buffers |
| Tree-child GC state | `children_by_tree`, markers, cascade | none, but full closure in journal | none |
| Blob fold work | root journals plus tree expansion | closure-delta stream | changed owner manifests |
| GC tree-body reads | first expansion of content tree | none for root closures | one manifest read per old/new owner |
| Hot-path journal bytes | tree hash | full transitive closure | compact owner transition |
| Pre-precommit tree debris | global tree debris/full GC | closure records avoid staged tree reads | namespace-scoped `_subtrees` sweep |
| S3 GC scratch | snap generations, retired/outcomes | coarse generations | coarse blob generations and manifest-delete bundles |
| Sharding blocker | target displacement in resident snap | solved by `RootEdgeIndex` | solved by root owner transitions |

Per changed owner transition, GC reads the affected full-tree manifest and emits blob deltas. This is
`O(changed manifest entries)`, not `O(pool)`, and it avoids writing the same transitive closure into
every root journal event.

## Debuggability And Resume {#debuggability-and-resume}

Every durable generation must answer:

- root shards read/skipped/minted and why;
- owner transitions folded;
- manifest ids activated/deactivated;
- manifest bytes and entry count;
- blob deltas emitted per target shard;
- retired blob candidates;
- retired manifest ids;
- orphan manifest sweep namespace/build prefix;
- fence positions;
- recheck results;
- exact-token delete outcomes;
- trim cursors.

A single blob delete should be explainable as:

```
blob B
  last source edge: root_namespace N, tree_instance_id T, path P
  owner transition: ref R removed T at root version V
  folded in generation G
  retired in round Rn with token Tok
  fenced through root version F
  rechecked indeg 0
  deleteExact(B, Tok) -> Deleted
```

Resume rules:

- root-local full-tree manifests are immutable; byte-identical existing writes may be adopted,
  divergent writes fail closed;
- generation files are write-once;
- manifest-delete work is keyed by `(round, generation, tree_instance_id)` and can be replayed;
- blob and manifest content deletes are exact-token and idempotent;
- orphan manifest sweep outcomes are compact and replayable;
- root cursors advance only with sealed blob-delta coverage;
- round visibility advances only after all retired sets and manifest-delete bundles are durable.

## Out Of Scope {#out-of-scope}

- No on-disk compatibility scaffolding for the old CA tree format.
- No content-addressed tree dedup.
- No nested root-local tree objects.
- No memoized closure object table.
- No full transitive closure in root journal records.
- No target-shard writes from the publish path.
- No removal of exact-token delete.
- No per-edge or per-candidate S3 object layout.

## Open Questions {#open-questions}

1. Exact debug fields in `FullTreeManifestProto` beyond the random 128-bit `tree_instance_id`.
2. Whether `_subtrees/<writer_id>/<build_seq>/` should be under the root namespace root directly or
   under a shard-specific subprefix tied to `root_shard`.
3. Internal full-tree manifest indexing: block size, optional directory index, and path lookup layout.
4. How much manifest streaming work is acceptable in `PromotePrecommit` revalidation and whether to
   store a compact blob summary inside the same manifest object.
5. Exact streaming protobuf framing shared by root manifests and full-tree manifest bodies.
6. How `fsck` should distinguish owner-visible missing manifest bodies from reclaimable
   pre-precommit manifest debris.
