---
description: Design for a streaming GC based on root-local part manifests and content-addressed blobs
sidebar_label: CAS GC part manifest redesign
sidebar_position: 1
slug: /superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design
title: CA GC root-local part manifest redesign design
doc_type: reference
---

# CA GC - root-local part manifest redesign - design {#ca-gc-root-local-part-manifest-redesign-design}

**Status:** design (2026-06-26, rev. 15). Branch `codex-gc-proposal-2026-06-26`.
**NOT behavior-preserving.** This is an architectural redesign of CA tree identity, build precommit,
read-path tree resolution, and GC accounting. CA is pre-release, so no persisted-data compatibility
path is required. Every behavior-changing phase is gated on a green `TLA+` model extension before code
lands.

rev. 14 (protocol tightening before TLA+): the root journal becomes a single ordered `RootOwnerEvent`
log; promotion is a pure owner move and a missing-body precommit is a non-activating, non-promotable
intent; `GenerationSeal` is split into the write-once `FoldSeal` and `CompletionSeal`; the lazy fence
is removed (only discovery and trim are lazy; the fence is always global); the recheck 404 policy is
made context-specific (matching fold); and the build prefix is modeled explicitly while
`(root_namespace_id, manifest_instance_id)` remains the unique safety identity.

rev. 15 (review fixes on rev. 14): a missing-body precommit is a **fold barrier** (the durable cursor
does not advance past it until it activates or is removed), so promotion is always of an activated
manifest and stays Δ=0; an **owner move** (`old.manifest_ref == new.manifest_ref`) records no blob
delta and no part-manifest cleanup; introduces `ManifestSafetyId` as the distinct TLA+ abstraction
term while `ManifestId = (root_namespace_id, ManifestRef)` stays the protocol identity; reconciles
`ViewableRound` (retired-token view after the retire barrier) with `FoldSeal`/`CompletionSeal`
(internal products / adoption after completion); tightens the missing-body precommit invariant.

The core change is now sharp: **only blobs remain content-addressed**. A root-local
immutable tree object is not a folder node and does not point to child tree objects. It is a
**part manifest**: one immutable object containing the complete logical subtree, including all
file paths, inline payloads, and blob references. Directories inside it are path prefixes or an
optional internal index, not protocol objects.

This removes the last recursive tree walk from the GC protocol. A root/precommit owner transition
names one part manifest through a structured `ManifestRef` (the journal carries the full ref,
not a bare nonce, so the read path and GC fold can address the object without a LIST/body scan). GC reads
that one manifest object as a stream and emits blob deltas. There are no nested tree instance ids, no
parent-child tree ownership, no tree expansion markers, no GC-side cascade state, and no full-closure
root journal records.

This design closes the remaining protocol gaps: the GC identity is namespace-qualified
`ManifestId = (root_namespace_id, ManifestRef)`; manifests are never shared between refs or root
namespaces; mutable per-ref payload stays in the root ref; missing manifest bodies under precommit
fail closed instead of requiring a new hot-path build-state object; `_manifests` is a reserved layout
segment; and the operation cost is stated as `O(changed manifest entries)`, not as a tree diff.
The format boundary is also explicit: `Protobuf` is for control-plane records and envelopes; hot
GC data-plane streams are dense block-framed sorted binary runs, not length-prefixed per-record
`Protobuf` streams.

## Goals {#goals}

The design must satisfy the original requirements:

- **R0: safety is non-negotiable and `TLA+`-provable.** No delay, race, duplicate worker, interrupted
  attempt, stale view, or backend reorder may make the system less reliable. `INV_NO_DANGLE`,
  `INV_NO_LOSS`, and `INV_NO_RETURN` must be proved, not argued.
- **R1: each GC round is bounded and streaming.** Work is proportional to changed root owner
  transitions and the entries of the manifests those transitions name. Reducers cancel unchanged blob
  edges after scatter. Memory is bounded by stream buffers. GC state is stored as coarse write-once
  objects.
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

## Terminology And Naming {#terminology-and-naming}

This document uses one strict naming rule: **`manifest` means only the immutable part manifest**. A
mutable root object is a root journal or root shard state, and a GC generation's completeness is
recorded by two write-once phase seals, the `FoldSeal` and the `CompletionSeal`. The current code still
contains legacy names such as `RootShardManifest`; the protocol names below are the names this redesign
should use.

| Name | Role | Naming verdict | Preferred name | Other acceptable names |
|---|---|---|---|---|
| `CA`, `CAS`, `ContentAddressed` | The content-addressed storage layer | `CAS` is common after expansion, but collides with compare-and-swap in low-level prose | `ContentAddressed` | `Content-Addressed Storage`, `CAS` after first expansion |
| `pool` | Shared object-storage prefix for one content-addressed store | Good, common, short | `pool` | `cas_pool` in schema fields when extra scope is useful |
| `blob` | Content-addressed payload object | Good and conventional | `blob` | none |
| `BlobId` | Strong type for blob identity | Acceptable; if identity is always a hash, `BlobHash` is more literal | `BlobId` | `BlobHash` |
| `Token` | Backend object incarnation token used by exact deletes | Too generic without context | `ObjectToken` | `BlobToken`, `BackendToken` |
| `deleteExact`, exact-token delete | Delete only if the observed token still matches | Good; describes the safety property | `deleteExact` in code, exact-token delete in prose | none |
| `TreeId`, `tree` | Legacy content-addressed static tree object | Bad for this design; carries the old recursive-tree/cascade model | no new tree object | `PartManifest`, `ManifestId` for the replacement |
| `part manifest`, `PartManifestProto` | Immutable static-folder body: paths, inline payloads, blob refs, optional index | Good; reserve `manifest` for this object only | `PartManifest` | `StaticFolderManifest`, `FileSetManifest` |
| `ManifestEntry` | One file entry inside a part manifest | Good; keeps the scope visible outside the part manifest body | `ManifestEntry` | `FileEntry` |
| `ManifestRef` | Compact root-journal reference to a part manifest, scoped by the owning namespace | Good after `manifest` is reserved for part manifests | `ManifestRef` | `PartManifestRef` |
| `ManifestId` | Namespace-qualified identity `(root_namespace_id, ManifestRef)` | Good, but always define its qualification | `ManifestId` | `QualifiedManifestId` |
| `manifest_instance_id` | Random 128-bit part-manifest instance component | Good; describes identity and avoids `nonce` jargon | `manifest_instance_id` | `manifest_random_id` |
| `writer_instance_id` | Writer incarnation component inside `ManifestRef` | Good; explicit that stable server id alone is not enough | `writer_instance_id` | `writer_epoch_id` |
| `build_sequence` | Monotone build sequence inside one writer incarnation | Good; avoids the non-common `seq` abbreviation | `build_sequence` | none |
| `RootNamespace` | Opaque namespace under `roots/` | Good | `RootNamespace` | `root namespace` in prose |
| `root` | Dynamic folder / namespace root | Too overloaded alone | `root namespace` | `root journal`, `root shard` when those are meant |
| `RootShardManifest`, root manifest | Mutable root-shard object with refs, mutable ref payloads, journal, fence state | Bad: mutable and journal-like, not a manifest | `RootShardState` | `RootJournalShard`, root journal |
| `root journal` | Mutable dynamic-folder journal and current refs | Good protocol name | `root journal` | `root shard state` for the object body |
| `RefRecord` | Current committed ref payload in the root journal | Acceptable, but `Record` is generic | `RootRef` | `RefEntry`, `RefRecord` in existing schema |
| `RefPayload` | Per-ref part manifest ref plus mutable files and publish timestamp | Good enough, but root scope can be clearer | `RootRefPayload` | `RefPayload` |
| `mutable_files` | Mutable per-ref metadata files | Acceptable, but scope should be explicit | `mutable_ref_files` | `mutable_files` in existing code |
| `JournalRecord` | Legacy add/remove reachability record | Too generic and tied to old tree deltas | `OwnerTransition` | `ReachabilityTransition` |
| `OwnerTransition` | Atomic old/new owner change in one root-journal decision | Good; describes the function | `OwnerTransition` | none |
| `PrecommitAdd`, `PrecommitRecord` | Build intent before committed publish | Good, common enough | `PrecommitAdd`, `PrecommitRecord` | none |
| `PromotePrecommit` | Atomic owner move from precommit to committed ref | Good; describes the operation | `PromotePrecommit` | `CommitPrecommit` |
| `_precommits` | Legacy separate precommit namespace | Bad for the new model; it should disappear | no new namespace | precommit records in the root journal |
| `_manifests` | Root-local storage prefix for part manifest bodies | Acceptable because `manifest` now has one meaning | `_manifests` | `_part_manifests` if layout clarity wins over brevity |
| `registry`, `rootsRegistryKey` | Authority for the root namespace universe | Good, but root scope should be explicit | `root registry` | `rootsRegistryKey` in code |
| `gc/state` | Mutable GC coordinator state | Good | `gc/state` | `GcState` in code |
| `gc_generation` | Write-once GC output generation | Good when scoped; plain generation is acceptable in prose when context is clear | `gc_generation` in schema, generation in prose | none |
| `FoldSeal`, `gc/gen/<generation>/fold_seal` | Write-once seal of what one generation folded (coverage, blob_target runs, cleanup bundles) | Good; one write-once object, never mutated | `FoldSeal` | fold seal |
| `CompletionSeal`, `gc/gen/<generation>/completion_seal` | Write-once seal of fence/recheck/delete/trim coverage and the generation-adoptable marker | Good; one write-once object, never mutated | `CompletionSeal` | completion seal |
| `RootOwnerEvent` | One ordered owner-change event (`old_binding`?/`new_binding`?) in the single root journal stream | Good; one event type replaces the old multi-record framing | `RootOwnerEvent` | owner event |
| `snap`, `GcSnap` | Legacy resident/durable in-degree snapshot | Bad abbreviation and carries `O(pool)` baggage | `BlobReachabilityState` | `InDegreeState`, `BlobTargetState` |
| `RetiredSet` | Condemned candidates with observed exact tokens | Good | `RetiredSet` | retired candidates |
| `OutcomeLog` | Durable delete outcomes | Good | `OutcomeLog` | delete outcome log |
| `fence` | Ordering barrier before delete recheck | Good and protocol-common | `fence` | root fence, registry fence when scoped |
| `recheck` | Fold-through-fence validation before delete | Good enough | `recheck` | fenced recheck |
| `trim` | Cursor/journal cleanup after sealed coverage | Good | `trim` | none |
| `cascade` | Legacy tree child-edge cleanup | Bad for the new model; keep only in history sections | no active step | owner removal emits blob decrements |
| `source edge` | Edge from `(ManifestId, path)` to a blob | Good if scoped | `blob_source_edge` | source edge in prose |
| `blob delta` | `+1` / `-1` update to blob reachability | Good | `blob_delta` | blob edge delta |
| `in-degree` | Number of active source edges to a blob | Good graph term | `blob_in_degree` in schema | in-degree in prose |
| `target shard` | Blob-hash reducer shard | Too vague alone | `blob target shard` | target shard when context is clear |
| `part_manifest_cleanup` | Cleanup work for an unowned part manifest | Good; explicit that it is cleanup, not reachability authority | `part_manifest_cleanup` | part-manifest cleanup work |
| `orphan_part_manifest_cleanup` | Cleanup of a part manifest body written before a visible owner and never activated | Good; scoped and less informal than debris | `orphan_part_manifest_cleanup` | orphan part-manifest cleanup |
| `RunFile`, `DataBlock`, `RunFooter` | Dense sorted binary data-plane stream | Good; `run` is common in LSM/SSTable designs | `RunFile`, `DataBlock`, `RunFooter` | `SortedRun` for the whole file family |
| `ViewableRound` | `TLA+` invariant: writers observe a round only after all retired sets are durable | Fine in the model, awkward in prose | `RoundVisible` in prose | `ViewableRound` in `TLA+` |
| `deadTok` | `TLA+` set of tokens that cannot return | Fine shorthand in the model, bad in prose | dead token set | `deadTok` in `TLA+` |

## Protocol Boundary {#protocol-boundary}

This is not a local GC optimization. It changes the formal object model, the root journal contract,
the read path, and the meaning of `fold`.

The `TLA+` model must change because the current model treats trees as content hashes with `treeEdges`,
`marker`, and a cascade action. This design needs a new model branch with:

- random 128-bit `manifest_instance_id` per part manifest;
- root-local immutable part manifest bodies addressed by a structured `ManifestRef`
  (`manifest_instance_id` + `writer_instance_id` + `build_sequence`);
- namespace-qualified `ManifestId = (root_namespace_id, ManifestRef)` as the only GC identity, with
  `RefMatchesBody` and `ManifestNamespaceMatches` checks;
- no nested tree objects and no child tree edges;
- `SingleManifestOwner` and `NoManifestIdReuse`;
- precommit owners, committed owners, and atomic `PromotePrecommit`;
- precommit-visible missing manifest bodies and missing blob leaves;
- blob in-degree derived only from active part manifests;
- part-manifest cleanup work keyed by `ManifestId`;
- namespace-scoped orphan part-manifest cleanup for objects written before `PrecommitAdd`;
- the existing global registry/root fence and fold-through-fence recheck.

The visible round skeleton intentionally stays close to the proved tail:

```
discover -> fold -> retire -> fence -> recheck -> exact-token delete -> trim
```

What changes is `fold`. Today it folds root ref records into root edges, expands content-addressed
trees, and later relies on cascade to remove child edges. This design folds root owner transitions, reads a
single root-local part manifest object for each old/new owner, and emits blob deltas directly.
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

The current code also shows where this design cuts complexity:

- `Build::stageTree` mints `TreeId` from a Merkle/content hash and retains encoded tree payload for
  re-upload. This design changes `TreeId` to a root-local part `ManifestId`; the optional payload digest
  is only integrity/debug data.
- `Store::readTree` assumes trees are global immutable content objects under `trees/<prefix>/<hash>`.
- This design reads part manifests from the owning root prefix instead.
- `RootShardManifest` is the current code name for a whole-object protobuf with `refs` and repeated
  `JournalRecord`. The protocol name is root shard state or root journal shard: it is mutable and must
  not be called a manifest in this design. The redesign still wants a streaming control-plane
  representation for the root journal, but the records are compact owner transitions, not full
  closures, and that framing is not reused for hot blob-scale reducer streams.
- `CasLayout` has global `blobKey` and `treeKey`. This design keeps `blobKey` and replaces CA `treeKey`
  with root-local part manifest keys.

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

This design keeps these proof obligations, but changes the tree model:

- tree identities are no longer content hashes;
- a part manifest is a single-owner root-local object;
- there is no tree-child protocol graph;
- only blob objects are content-addressed, deduplicated, and shared across roots;
- a `ManifestId` is never reused after it becomes visible in any root journal.

## Core Principle {#core-principle}

There are only two protocol-level folder concepts:

- **Dynamic folders** are root namespaces with root journals. They have a mutable journal and own
  top-level part manifests through owner transitions.
- **Static folders** are part manifest objects. They are immutable and indivisible. Their body is
  the complete subtree: every file path, every inline value, and every blob reference under that root.

The reference graph is therefore:

```
dynamic root/precommit ref -> root-local part manifest
part manifest         -> content-addressed blob
```

Directories inside a part manifest are not separate objects. They are represented by path
prefixes. If lookup/list performance needs it, the same manifest object may include an internal
directory index, but that index is still part of the single immutable manifest payload.

Only blobs are content-addressed. Part manifests are not deduplicated by payload. If two parts
have byte-identical logical metadata, they still get different `ManifestId`s. The payload may carry a
digest for corruption detection, but that digest is not identity and is never used as a GC key.

This is the key simplification: stale work is keyed by a unique `ManifestId`, not by a content
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

### Part Manifest Reference And Identity {#part-manifest-reference-and-identity}

A root journal stores a compact `ManifestRef`. `CasLayout` derives the object key from that ref and the
owning root namespace. Keeping the ref structured, not a string key, keeps journal authority free of
path strings and lets the layout evolve in `CasLayout`:

```
ManifestRef {
    writer_instance_id        // writer incarnation, e.g. server_id/epoch or random writer_instance_id
    build_sequence        // build-scoped debris cleanup; part of identity
    manifest_instance_id   // random 128-bit; gives collision-safety and the never-reused guarantee
}
```

The protocol identity used by GC is namespace-qualified:

```
ManifestId = (root_namespace_id, ManifestRef)
```

This `ManifestId` is the **protocol identity** used to locate/address objects and key source edges and
cleanup work; it is distinct from the `ManifestSafetyId` the TLA+ model abstracts it to (see
[Abstraction Boundary For The Model](#abstraction-boundary-for-the-model)).

`CasLayout` builds the key as
`<pool>/roots/<root_namespace>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`, with
`<aa>` derived from `manifest_instance_id`. Source edges, blob deltas, part-manifest cleanup work,
owner sets, and `NoManifestIdReuse` are all keyed by `ManifestId`, not by a bare `ManifestRef`.
`root_namespace_id` is **not** stored in the ref carried by the root journal; it comes from the owning
root context.

This distinction is load-bearing. Two namespaces may legally have the same
`(writer_instance_id, build_sequence, manifest_instance_id)` tuple without addressing the same object
key. A reducer that merged by `ManifestRef` alone could combine unrelated source edges or cleanup work
across namespaces.

Requirements:

- `manifest_instance_id` is random and never derived from payload;
- no visible `ManifestId` is ever reused;
- manifest objects are immutable write-once objects;
- a part-manifest instance-id collision fails closed before any root journal transition becomes
  visible.

The manifest body repeats its own `ManifestRef` and `root_namespace_id`, used only for fail-closed
validation — never as a second identity:

```
PartManifestProto {
    header
    ref                  // ManifestRef: writer_instance_id, build_sequence, manifest_instance_id
    root_namespace_id
    payload_digest       // integrity/debug only; not a key, not dedup, not in-degree
    repeated ManifestEntry entries
    optional DirectoryIndex directory_index
}

ManifestEntry {
    path
    placement = inline | blob
    blob_hash
    blob_size
    inline_bytes
}
```

This is a logical schema, not a requirement to encode every `ManifestEntry` as an independent
length-prefixed `Protobuf` record. For large manifests, `entries` use the same block-framed entry
stream described in [Backpressure And Journal Encoding](#backpressure-and-journal-encoding). The
header, debug fields, footer, and optional indexes may still be `Protobuf` envelopes.

Entries are canonicalized by path. Duplicate paths are corruption. Duplicate blob hashes across
different paths are allowed and are folded by deterministic source edge ids `(ManifestId, path)`.

Two checks make a ref trustworthy, both modeled as invariants and fail-closed at read/fold time:

- `RefMatchesBody`: the journal `ManifestRef` equals the `ref` inside the decoded manifest body; a
  mismatch means the ref is addressing the wrong object.
- `ManifestNamespaceMatches`: the body `root_namespace_id` equals the owning root namespace; a mismatch
  would be a cross-namespace dangle and would hand the debris sweep the wrong authority.

### Part Manifest Ownership {#part-manifest-ownership}

A visible part manifest has at most one structural owner, identified by `ManifestId`:

- a committed root ref;
- a precommit ref.

There is no parent-tree owner because there are no nested tree objects. Promotion moves ownership from
a precommit ref to a committed root ref. It must be one root-shard CAS transition, not "add table ref
now, best-effort remove precommit later". The current two-namespace best-effort precommit removal is
safe in the old model because duplicate edges only over-protect; this design chooses the simpler
invariant instead: a manifest owner is singular and explicit.

An implementation may represent the move as `old_owner -> new_owner` inside one journal record. It
must not expose a state where neither owner exists.

Manifests are never shared across refs and never shared across root namespaces. If a part must appear
in another table or root namespace, the destination publishes a new part manifest with a fresh
`ManifestId` over the same content-addressed blob hashes, then the source ref is dropped if the
operation is a move. Blob dedup gives the storage win; manifest metadata is deliberately duplicated to
keep ownership local and singular.

`Atomic` database `RENAME TABLE` is namespace-stable for CA because the storage root is UUID-keyed. It
does not move the CA folder, does not change `ManifestId`, and does not need a manifest operation.
Legacy or cross-table republish paths are ordinary destination publishes plus source drops; there is
no cross-namespace manifest move primitive.

## S3 Layout {#s3-layout}

Blobs stay global:

```
<pool>/blobs/<aa>/<blob_hash>
```

Root-local part manifests live under the owning root prefix:

```
<pool>/roots/<root_namespace>/
  <root_shard_number>
  _files/...
  _manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto
```

`_manifests` is a reserved segment, like `_files` (renamed from the earlier `_subtrees` name because
the object is one immutable part manifest, not a nested subtree).

`_manifests` must be rejected by `CasLayout::checkNamespace` as a namespace segment. Namespace/root
listers must not expose it as a user namespace; registry-based namespace discovery already makes that
the normal path, but raw LIST helpers must preserve the same rule.

The `<writer_instance_id>/<build_sequence>` prefix doubles as build-scoped debris cleanup and diagnostics.
`writer_instance_id` must identify the writer incarnation, not only the stable server id, so a new process epoch
does not reuse the same build prefix. `manifest_instance_id` is the random 128-bit collision-safety field.
The GC identity is `ManifestId`, while the root journal carries the compact `ManifestRef`. The `<aa>`
fanout is derived from `manifest_instance_id`, not from the payload digest.

For the user's example namespace, the layout can be:

```
server/store/ab/uuid@cas@/
  17
  _files/...
  _manifests/srv-a/1042/7f/7f3a...c1.proto
```

where `17` is a root shard manifest and `_manifests/srv-a/1042/7f/7f3a...c1.proto` is a root-local
part manifest staged by build sequence `1042`.

Important: the build-scoped prefix is not a deletion authority. A manifest can remain live after the
build sequence is below the watermark because promotion keeps the same `ManifestId` and object. Cleanup
must check liveness per `ManifestId` against a sealed namespace owner view; it must never delete a whole
build prefix blindly.

## Root Journal Format {#root-journal-format}

The root journal no longer carries transitive closures, and it no longer carries separate
`OwnerTransition` / `PrecommitTransition` / `PromotePrecommit` record types. It is **one ordered
`RootOwnerEvent` stream** (plus the committed `RefRecord` payloads), folded in `transition_version`
order. Every owner change — create/abandon precommit, publish/drop/repoint a committed ref, and
promote a precommit — is one `RootOwnerEvent` that removes at most one owner binding and adds at most
one owner binding:

```
RefRecord {
    ref_name
    manifest_ref       // ManifestRef, interpreted inside this root namespace
    mutable_files      // e.g. txn_version.txt, metadata_version.txt, uuid.txt
    published_at_ms
}

RootOwnerEvent {
    transition_version
    old_binding?    // { owner, manifest_ref } : the owner binding removed
    new_binding?    // { owner, manifest_ref } : the owner binding added
}

Owner = committed(ref_name) | precommit(build_id, final_ref_name)
```

Semantics, all expressed as one `RootOwnerEvent` (`T` is the manifest's `ManifestRef`):

- create precommit: `old = none`, `new = { precommit(build_id, final_ref_name), T }`;
- abandon/reclaim precommit: `old = { precommit(build_id, final_ref_name), T }`, `new = none`;
- publish committed without precommit: `old = none`, `new = { committed(ref_name), T }`;
- drop ref: `old = { committed(ref_name), T }`, `new = none`;
- repoint ref: `old = { committed(ref_name), T_old }`, `new = { committed(ref_name), T_new }`;
- **promote precommit: `old = { precommit(build_id, final_ref_name), T }`,
  `new = { committed(final_ref_name), T }` — same `manifest_ref` `T`, blob Δ = 0 (a pure owner
  move).**

Promotion is not a distinct record kind. It is one `RootOwnerEvent` whose `old_binding` and
`new_binding` name the **same** `manifest_ref`, moving ownership from precommit to committed without any
blob-edge change.

Every binding that names a manifest carries a `ManifestRef`, never a bare nonce. The reader and GC fold
combine that ref with the owning root namespace to form `ManifestId` and address the object directly
via `CasLayout` without a LIST or body scan.

Mutable per-ref payload stays in `RefRecord`. `mutable_files`, `published_at_ms`, and similar
non-reachability metadata are not part of `PartManifestProto`. A mutable-only update is a
root-shard CAS that changes `RefRecord` payload, emits no owner transition, emits no blob delta, and
does not change `ManifestId`.

The current `JournalRecord{Add, Remove}` shape is insufficient for target-sharded streaming because
the target reducer cannot infer the displaced old tree. The single `RootOwnerEvent` encodes
replace/move semantics at the root source through its paired `old_binding`/`new_binding`. With that,
rev. 5's durable `RootEdgeIndex` is no longer a required long-lived GC object: root-shard mappers can
emit paired old/new deltas from one source event.

The planned streaming root-journal migration still matters, but it is a control-plane format. A root
journal should be readable as a bounded record stream so GC can fold the single ordered
`RootOwnerEvent` stream without materializing the whole root shard state in memory:

```
RootJournalHeader
RefRecord...
RootOwnerEvent...        // one ordered stream, folded in transition_version order
RootJournalFooter
```

This does not imply length-prefixed per-record `Protobuf` for blob-scale data streams. Target-shard
reducer inputs/outputs, blob in-degree runs, blob delta runs, source-edge runs, retired-candidate
streams, and large manifest entry streams use dense block-framed sorted binary runs.

## Read Path Scope {#read-path-scope}

This redesign changes the query-hot read path.

`Store::resolveRef` resolves a ref to a root-local `ManifestRef` (`manifest_instance_id`, `writer_instance_id`, and
`build_sequence`), its owning root namespace, and the mutable per-ref payload. `Store::readTree` no
longer reads `trees/<hash>`. It derives the key from `ManifestId` via `CasLayout`, reads the part
manifest, verifies `RefMatchesBody` and `ManifestNamespaceMatches`, then answers path lookup or
directory listing from the manifest entries and optional directory index.

The old per-process tree decode cache shared by content hash becomes less useful because each publish
uses a unique `ManifestId`. That is an intentional tradeoff. The data that matters for storage and
large reads is still blob-deduplicated; tree metadata is small, immutable, and can be cached by
`(ManifestId, token)`.

Planning must include:

- path lookup over a part manifest;
- optional directory index for fast `listDirectory`-style operations;
- bounded decode/cache memory;
- fail-closed behavior when a committed ref names a missing manifest.

## Build And Precommit Protocol {#build-and-precommit-protocol}

Precommit should live in the target root prefix and follow the same ownership rules as committed refs.
The semantic relaxation is: **precommit may reference a missing manifest body or missing blob bodies;
committed refs may not**. A missing precommit manifest body protects nothing and cannot be promoted;
it is an intent record that fails closed.

### Stage Part Manifest {#stage-part-manifest}

`Build::stageTree` changes from "compute Merkle `TreeId`" to "mint root-local part `ManifestId`".

The writer builds one part manifest:

1. Resolve the complete logical subtree into canonical entries: full path, placement, blob hash/size or
   inline bytes.
2. Mint one random 128-bit `manifest_instance_id` (with `writer_instance_id`/`build_sequence` it forms the `ManifestRef`;
   together with the root namespace it forms `ManifestId`).
3. Stream-write the manifest object under `_manifests/<writer_instance_id>/<build_sequence>/...`, normally with
   `putIfAbsentStream` and no preliminary `HEAD` because the nonce is random.
4. Keep only a bounded stream buffer, temp-spool handle, or source descriptor needed for retry. The
   encoded manifest payload does not have to stay resident after a successful object write.

The happy path writes the manifest object before `PrecommitAdd`, but safety does not rely on an atomic
body-exists check before the root-shard CAS. If the manifest body is absent when GC folds the
precommit, the precommit contributes no blob edges and is recorded as a missing-body intent. If the
manifest body is absent at promotion, promotion fails closed and the writer retries with a fresh
`ManifestId`. Blobs named by an existing precommit manifest may also be absent.

This is the replacement for `JournalRecord.closure`: the part manifest itself is the staged
structure. GC never needs a full closure copied into the root journal, and it never needs recursive
reads to reconstruct a staged precommit. The price is that a missing precommit manifest body is a
fail-closed non-protecting intent, not corruption.

### Pre-Precommit Part-Manifest Debris {#pre-precommit-part-manifest-debris}

Manifest bodies are written before `PrecommitAdd`. If a build stops in that window, no owner
transition names those `ManifestId`s. Owner-driven GC would never read or delete them unless the
protocol adds an explicit debris path.

This section defines the build-side contract. The round-level cleanup algorithm is specified in
[Orphan Part-Manifest Cleanup Sweep](#orphan-part-manifest-cleanup-sweep).

This design makes this a bounded space-liveness obligation, not a hot-path safety dependency:

- writer abort does best-effort deletion of its own `_manifests/<writer_instance_id>/<build_sequence>/` objects;
- GC has a rare backstop sweep for build-scoped `_manifests` objects only when the writer incarnation
  can no longer publish new precommits from that prefix;
- sweep eligibility may come from a durable watermark fact such as `(epoch matches and min_active >
  build_sequence)`, an explicit retired epoch sentinel, or a replaced writer incarnation; it must not come
  from a frozen-seq / judged-dead heuristic alone;
- the sweep is scoped to one root namespace at a time;
- the sweep builds the active `ManifestId` set from that namespace's sealed root/precommit owner view;
- the sweep deletes only eligible-prefix manifest objects whose `ManifestId`s are absent from that
  namespace active set;
- every manifest delete is exact-token;
- if a writer races a conservative sweep and publishes a precommit whose body is gone, GC treats it as
  a missing-body precommit and promotion fails closed.

This is a space-liveness mechanism, not reader-facing correctness. A pre-precommit manifest has no root
owner, so readers cannot reach it. The important rule is that the debris sweep must never treat
"dead build prefix" as enough to delete. It needs conservative prefix eligibility, a sealed
per-namespace owner view, and a per-object liveness check.

The common case should be writer cleanup. The GC backstop is for stopped writers and must be bounded:
one namespace, one eligible prefix, and a limited number of exact-token deletes per round.

### Final Ref Name Requirement {#final-ref-name-requirement}

The normal precommit path requires `final_ref_name` up front. In `MergeTree`, the output part name is
known before the build: insert assigns it and merge/mutation knows its target. That lets
`PrecommitAdd` live in the same root shard as the final committed ref, so `PromotePrecommit` is one
root-shard CAS owner move.

This design deliberately does not include a GC-visible scratch-root fallback. A caller that cannot know
`final_ref_name` must delay `PrecommitAdd` until the final name is known or get a separate modeled
design. A hidden scratch precommit would double-write manifests and add another debris location to
prove.

### Precommit Add {#precommit-add}

The precommit CAS is written to the same root namespace as the future committed ref. For a normal part
publish, the precommit is placed in the same root shard as `final_ref_name`, so promotion can be atomic
in one shard CAS.

`PrecommitAdd` does not need a body-exists `HEAD` as a safety authority. Such a `HEAD` would still be
racy with orphan sweep before the root-shard CAS. Instead, the CAS only has to write the intent
record. GC and promotion handle a missing precommit manifest body by failing closed. Implementations
may still verify body presence as an optimization or diagnostic, but a positive result is not a proof
input.

The precommit record:

- names `build_id`;
- names `final_ref_name`;
- names the part `ManifestRef`, interpreted as `ManifestId` inside the owning root namespace;
- records enough build watermark identity to let GC reclaim abandoned precommits;
- does not carry transitive closure.

After `PrecommitAdd` is visible, the precommit is in exactly one of two states, decided at fold time:

- **activated** — the manifest body can be read and passes `RefMatchesBody` /
  `ManifestNamespaceMatches`. Its blob edges are emitted at fold and protect every blob hash it names:
  all currently present blobs and also future uploads of the same blob hashes, because the edge already
  exists in the target in-degree state. Only an activated precommit is promotable.
- **non-activated intent** — the manifest body is missing or invalid. It emits no blob edges, protects
  nothing, and is **not promotable**. It is a fail-closed intent, not corruption.

On the happy path the body exists before `PrecommitAdd`. A missing body makes the precommit a
non-activating **barrier**: the durable fold cursor is **not folded past it** (see
[Fold Owner Transitions](#fold-owner-transitions)), so it is never recorded as a folded-past
non-activated record that a later promotion could read as a pure owner move.

A non-activated precommit cannot be promoted. The writer recovers by re-staging with the body present —
that is, by creating a **fresh `ManifestId`** (which activates) — never by promoting the
missing-body intent.

### Blob Uploads Under Precommit {#blob-uploads-under-precommit}

The modeled order is:

1. write the root-local part manifest body;
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

- precommit manifest body should exist before precommit is visible on the happy path, but a missing
  body under precommit is a legal fail-closed, non-activating intent;
- blob bodies may be missing while the owner is precommit;
- a blob that is present and reachable from an activated precommit (readable, validated manifest body)
  must not be deleted;
- a precommit whose manifest body is missing is a non-activated intent: it contributes no blob
  in-degree and cannot be promoted (the writer must re-stage with a fresh `ManifestId`);
- a missing blob under precommit is not corruption.

This matches the existing `CaBuildRootPrecommit.tla` split between build-root intent and committed
reader-facing truth.

### Promote Precommit {#promote-precommit}

Commit promotion is a single root-shard CAS over an **already-activated** precommit:

1. Refresh retire view if the root shard or registry fence demands it.
2. Stream-read the precommit manifest body and validate `RefMatchesBody` /
   `ManifestNamespaceMatches`.
3. Revalidate every blob leaf listed in the precommit manifest.
4. If the manifest body is absent (a non-activated precommit), a blob is absent, or a blob is condemned
   and not recreatable from writer source, fail closed with `ABORTED`. A non-activated precommit is
   never promoted; the writer re-stages with a fresh `ManifestId`.
5. Atomically replace `precommit(build_id, final_ref_name)` owner with `committed(final_ref_name)` owner.
6. Append the promotion `RootOwnerEvent` (same `manifest_ref` in `old_binding` and `new_binding`) to
   the root journal.

**Promotion NEVER emits blob deltas.** It is only an owner move (precommit → committed) on an
already-activated manifest, so blob Δ = 0. The dynamic owner changes; the static manifest and its blob
edges do not. The single ordered journal guarantees the activating `PrecommitAdd` is folded before the
promotion, so there is no "was this precommit active when folded earlier?" state on the promote path.

The fold barrier makes this guarantee durable, not merely ordered: GC never advances the fold cursor
past a missing-body precommit (see [Fold Owner Transitions](#fold-owner-transitions)), so a promotion —
which requires the precommit binding still present — is **always of an activated manifest**. The `+1`
source edges were therefore already emitted at activation, and re-emitting them at promote would
double-count. Promotion correctly stays a pure Δ = 0 owner move and re-emits no edges.

This is simpler than the current `Build::publish` plus best-effort precommit `Remove`: there is no
leftover stale precommit edge after a successful commit, and no second namespace that GC must discover,
fence, and reclaim.

Promotion revalidates the whole manifest. That is preexisting safety logic from the build-root model,
not a new weakness. It is `O(manifest entries)` and one streaming manifest read. If it becomes visible
in profiles, the optimization is an internal per-manifest blob summary or directory/blob index, still
inside the same immutable manifest object.

### Write Path Budget {#write-path-budget}

Happy-path write budget for an already-registered namespace, no root-shard CAS conflict, and `U`
unique blob hashes in a manifest:

- part manifest: one streaming conditional create (`putIfAbsentStream`), no preliminary `HEAD`
  for the random nonce;
- `PrecommitAdd`: one root-shard read and one root-shard `casPut`;
- blob materialization: one streaming `PUT` per new blob; for a likely dedup hit, one `HEAD` and no
  body upload when the token is live;
- `PromotePrecommit`: one root-shard read, one streaming manifest read, up to `U` blob `HEAD`s for
  fail-closed revalidation, and one root-shard `casPut`;
- first publish into a namespace still pays the existing registry `GET` / `casPut` once.

The proof-critical operations are `PrecommitAdd`, final manifest read, final blob revalidation, and
atomic `PromotePrecommit`. The manifest body `HEAD` before `PrecommitAdd` is not proof-critical and
should not be required. Dedup-cache `HEAD`-first behavior is an optimization: a stale cache entry can
only add a `HEAD`, not publish a dangle.

Memory on the write path must be bounded by stream buffers plus manifest construction state. Large blob
payloads should stay as temp-file/source descriptors until the final revalidation window closes; they
must not be copied into one resident `String`. A writer that needs to resurrect a condemned blob uses
its own source stream, never a backend `GET` of the condemned object.

CPU cost is dominated by payload hashing and canonical manifest encoding. If the builder already emits
entries in canonical path order, manifest encoding can stay streaming. Otherwise the implementation
must account for the sort cost and enforce `manifest_entries` / `manifest_encoded_bytes` caps before
publishing any owner transition.

### Abandon Or Reclaim Precommit {#abandon-or-reclaim-precommit}

An abandoned precommit owner is removed by GC or by writer cleanup:

```
PrecommitRemove(build_id, manifest_ref)
```

The `ManifestId` is unique and never reused. Therefore removing this owner later is safe even if a
different build later creates a byte-identical manifest under a different id.

If GC falsely reclaims a still-live build's precommit, the later `PromotePrecommit` must fail closed
because `precommit(build_id)` is no longer present. The writer retries by creating a fresh precommit
with a fresh `ManifestId`, then revalidates blobs again.

## GC Authority Model {#gc-authority-model}

GC tracks blob reachability from active part manifests:

```
blob_indeg[b] =
    Cardinality({
        source edge (ManifestId, path)
        : ManifestId is active
          and its entry at path references blob b
    })
```

Part manifests are not target-counted like blobs. Their liveness is structural:

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

GC still maintains a streaming blob in-degree generation. It may also maintain a coarse part-manifest
cleanup work bundle for `ManifestId`s whose owner was removed. That bundle is keyed by `ManifestId`
and round, not by content hash, and is safe to replay.

## Round Protocol {#round-protocol}

The round remains recognizable:

```
discover -> fold owner transitions -> retire -> fence -> recheck -> exact-token delete -> trim
```

There is no GC-side cascade step.

"No cascade" here means no deferred child-edge state and no revival-prone tree-hash-keyed child-edge
removal. It does **not** mean the ordering obligation disappears. For an owner removal, GC must read
the part manifest while it is still available, seal the corresponding blob decrements into the
generation, and only then allow exact-token deletion of the manifest body. The old cascade ordering
becomes a simpler revival-proof ordering on unique `ManifestId`s.

### Discovery {#discovery}

`gc/registry` remains the authority for the namespace universe. LIST is only an accelerator:

- listed root token equals persisted folded token: skip body read;
- token missing, ambiguous, stale, or unsupported: read the root shard body;
- registry namespace missing from LIST: read/mint according to current registry rules;
- LIST never shrinks the registry universe.

The token-diff optimization from rev. 5 still applies, but it is now even smaller: changed root shards
contain compact `RootOwnerEvent`s, not full closures.

### Fold Owner Transitions {#fold-owner-transitions}

For every changed root shard, fold the **single ordered `RootOwnerEvent` stream** in
`transition_version` order. Each event removes at most one `old_binding` and adds at most one
`new_binding`; the ordered fold guarantees an activating `PrecommitAdd` is seen before any promotion of
it, so no per-event "was this precommit active earlier?" state is needed.

`RootOwnerEvent` handling is defined up front by comparing `old_binding.manifest_ref` and
`new_binding.manifest_ref`:

- **both present and EQUAL ⇒ OWNER MOVE** (e.g. promote precommit→committed): **no blob delta, no
  part-manifest cleanup** (the manifest stays owned; only the owner kind changes).
- **old present, `manifest_ref` no longer owned after the event (true removal) ⇒** emit `−1` for its
  source edges AND record part-manifest cleanup keyed by that `ManifestId` (single-owner, so a removed
  binding with no equal new binding is a true removal).
- **new present, no equal old (activation) ⇒** emit `+1` (subject to the FIX-1 body-present barrier
  below).

**Fold barrier (missing-body precommit).** GC fold does **NOT** advance the durable fold cursor past a
`RootOwnerEvent` that leaves a live precommit binding whose manifest body is not yet present and valid.
Each round re-reads that body; the cursor advances only once (i) the body is present+valid → the
precommit **ACTIVATES** and emits its `+1` source-edge deltas, or (ii) the precommit binding is removed
(abandon/reclaim → no edges). Therefore every precommit the fold has advanced past is either activated
or gone. A promotion requires the precommit binding still present, so it is always of an activated
manifest → it stays a pure Δ=0 owner move. **Liveness:** a stuck missing-body precommit is bounded by
the watermark-based precommit reclaim, which removes it (a removal event) and unblocks the cursor; fold
may collapse an add+remove of a never-activated precommit as a no-op.

For `old_binding` (its `manifest_ref`, interpreted as `ManifestId` in the owning root namespace):

- if the binding was a committed owner, or a precommit that was **activated** when folded, emit `-1`
  blob-edge deltas for every blob entry — using the manifest body if still readable, or the blob edge
  list already sealed at fold; if neither is available, fail closed rather than guessing;
- if the binding was a precommit that was a **non-activated** (missing-body) intent, emit no blob
  deltas (it never contributed any);
- for an edge-emitting removal, derive the key via `CasLayout`, verify `RefMatchesBody` and
  `ManifestNamespaceMatches` (else fail closed), and stream its entries once;
- an old binding records `−1` decrements and part-manifest cleanup **ONLY** when it is a true removal
  (`old.manifest_ref != new.manifest_ref`); an owner move (equal refs) records neither.

For `new_binding` under a **committed** owner (its `manifest_ref`, interpreted as `ManifestId` in the
owning root namespace):

- derive the key via `CasLayout`, verify the manifest object exists, and check `RefMatchesBody`
  and `ManifestNamespaceMatches`;
- stream its entries once;
- emit `+1` blob-edge deltas for every blob entry.

For `new_binding` under a **precommit** owner (this is the activation decision):

- derive the key via `CasLayout`;
- if the body exists and validates, the precommit **activates**: stream it once and emit `+1`
  blob-edge deltas;
- if the body is absent or invalid, record a **non-activated** precommit intent and emit no blob
  deltas;
- do not treat the missing body as reader-facing corruption, because precommit is not reader-facing.

A **promotion** event (`old_binding` and `new_binding` name the same `manifest_ref`, moving
precommit → committed) emits **no blob deltas**: the manifest is already activated and the same edges
stay in force. Promotion of a **non-activated** precommit is rejected fail-closed — there is no branch
that treats promotion as a committed new-binding add, because the writer must instead re-stage with a
fresh `ManifestId` (which activates) before any committed binding can become visible. This eliminates
by construction the old hazard of a promote that adds `+1` deltas after a missing-body precommit.

The fold is streaming. It keeps only a manifest record buffer and target-sort/spill buffers. It does
not build a resident closure set and does not recursively read any child manifests. Duplicate blob
references are handled by deterministic source edge ids `(ManifestId, path)`.

A repoint/mutation over a large manifest emits `-old` and `+new` by reading both part manifests. Many
unchanged blob refs may cancel in the target reducer. The design deliberately avoids a tree-diff
protocol: manifests are metadata, writes stay compact, and the reducer handles cancellation in
background.

Fold output is sealed in a write-once `FoldSeal`. A generation has two write-once phase seals, neither
ever mutated after its single write:

```
gc/gen/<generation>/fold_seal             // written once at fold completion
gc/gen/<generation>/completion_seal        // written once at completion
gc/gen/<generation>/blob_target/<target_shard>/...
gc/gen/<generation>/part_manifest_cleanup/<owner_shard>/...
```

`FoldSeal` (`gc/gen/<gen>/fold_seal`) records, per `(ns, shard)`, which root events were folded —
its classification, `folded_token`, and `folded_cursor` coverage — plus the `blob_target` runs and the
`part_manifest_cleanup` bundles produced. The `SabotageCutOverclaim` defense is carried on these
`FoldSeal` coverage fields: a cursor/token may never be sealed past an unsealed `RootOwnerEvent` delta.
`CompletionSeal` (covered in [Recheck And Delete](#recheck-and-delete) and [Trim](#trim)) records the
fence positions, recheck coverage, exact-token delete outcomes, trim coverage, and the "generation
adoptable" marker.

These are coarse files. There is no object per edge, manifest, or candidate.

### Retire {#retire}

Blob retire scans the touched blob target shards and emits candidates whose in-degree transitioned to
zero. It keeps the existing per-candidate `HEAD` until a separate model proves a token-source
optimization.

Part-manifest retire consumes part-manifest cleanup work:

- read each manifest object token, or reuse the token captured during fold if the backend proof covers
  it;
- write a compact retired-manifest bundle;
- do not delete the manifest object until the fence/recheck phase confirms the owner removal.

Part-manifest delete is space cleanup, not reader-facing correctness. If a manifest delete is delayed, the
manifest is unreachable debris. It must not keep blob in-degree elevated after its owner removal has
been folded.

### Orphan Part-Manifest Cleanup Sweep {#orphan-part-manifest-cleanup-sweep}

The regular owner-transition fold cannot see manifest bodies written before `PrecommitAdd`. A bounded
background sweep handles those objects. This is the GC-side backstop for
[Pre-Precommit Part-Manifest Debris](#pre-precommit-part-manifest-debris), not a second cleanup
protocol.

The sweep is **per namespace**, not pool-wide:

- choose one root namespace and one build prefix `_manifests/<writer_instance_id>/<build_sequence>/` whose writer
  incarnation is conservatively sweep-eligible;
- eligibility must mean the writer incarnation can no longer write that prefix, for example because
  the same epoch's durable watermark has `min_active > build_sequence`, the epoch is explicitly retired, or
  the writer incarnation has been replaced;
- do not use a frozen-seq / judged-dead heuristic alone as deletion authority for manifest bodies;
- build the active `ManifestId` set from that namespace's sealed committed-ref and live-precommit owner
  view;
- enumerate that one build prefix;
- delete only manifest objects whose ids are absent from the active set, using exact tokens;
- record compact outcomes so the sweep can resume without depending on LIST stability.

This sweep is not allowed to emit blob decrements, because a pre-precommit manifest never contributed
blob increments. If a `ManifestId` is found in the owner view, it leaves the sweep and is handled only
by the normal owner-transition path.

### Retire Visibility Barrier {#retire-visibility-barrier}

`gc/state.round` advances only after every blob target shard's retired set and every part-manifest
cleanup bundle for the round are durable. This preserves `ViewableRound`: a writer refreshing to round
`R` sees the complete retired-token set, not a subset produced by faster reducers.

Visibility splits in two. The **retired-token view must be writer-visible after this retire barrier**,
because writers need it for their publish gate before the destructive-delete proof closes; that is the
view `ViewableRound` governs. The GC's internal mapper/reducer **products** and the generation
**adoption** stay internal until the `CompletionSeal` is written. So a round's retired-token view is
published after the retire barrier, while products and adoption follow `FoldSeal`/`CompletionSeal` (see
[Sharding Model](#sharding-model)).

### Global Fence {#global-fence}

The fence is **always global** and coordinator-owned. There is no "lazy fence" and no model-proven
lazy subset: the fence is never skipped for any shard.

1. fence `gc/registry`;
2. fence **every** root shard in the fence universe;
3. record fence positions in the `CompletionSeal`.

The fence is load-bearing. A publish into one root shard can protect blobs in any target shard, so
target reducers do not own independent fences. The rationale for keeping the fence global is that a
writer must be guaranteed to observe the new round / retired view before publishing into any shard, and
there is no atomic writer-visible fence authority that a token-diff classification could substitute for.
Discovery (token-diff in Phase 2) and trim are the only laziness in the protocol.

### Recheck And Delete {#recheck-and-delete}

Recheck folds the ordered `RootOwnerEvent` stream through the durable fence positions into the
completion generation. Then:

- a blob candidate is deleted only if its blob in-degree is still zero and the exact token still
  matches;
- a manifest is deleted only if the `ManifestId` still has no committed/precommit owner in the
  fold-through-fence view;
- every delete is exact-token;
- token mismatch is spared/replaced, not destructive;
- absent under a held retired token is an idempotent confirmed outcome.

Recheck applies the **same context-specific 404 policy as fold**; there is no blanket "missing manifest
body ⇒ spare" rule:

- a missing or invalid **committed or promoted new-binding** manifest body in the fence window
  **clamps/aborts the affected delete** (fail-closed, not spare-by-default) and is surfaced to `fsck` —
  a committed ref must never resolve to a missing manifest;
- a missing **precommit** body is **non-activating** (it contributes no edges), not corruption;
- an **old-binding removal** uses the blob edges **already sealed at fold** (computed while the old body
  was still required present) or fails closed — recheck must never need to read a deleted manifest body
  to compute decrements.

The blob decrements for any removal were already produced during fold from the manifest body while it
was still required present. If the process is interrupted after a manifest object delete but before
metadata cleanup, the part-manifest cleanup bundle is enough to resume outcome handling.

Recheck coverage, the fence positions, the exact-token delete outcomes, trim coverage, and the
"generation adoptable" marker are written once into the `CompletionSeal` (`gc/gen/<gen>/completion_seal`),
which is never mutated after that single write.

### Trim {#trim}

Trim is lazy. Trim root journals only below the `folded_cursor` coverage recorded in the `FoldSeal`,
and record the trim coverage in the `CompletionSeal`. Trim part-manifest cleanup work only after:

- blob deltas from the owner removal are incorporated into the durable blob generation;
- manifest exact-token outcomes are durable or the work is explicitly carried forward;
- retired sets/outcomes are handled according to the existing drop-on-confirmed-outcome rule.

`INV_JOURNAL_COVERAGE` remains mandatory: a recovery from the parent generation must be able to replay
every `RootOwnerEvent` not already incorporated into an adopted sealed generation.

## Sharding Model {#sharding-model}

Default `gc_shards = 1`.

For sharded mode:

- root-shard mappers stream owner transitions and scatter blob deltas by blob hash;
- blob target reducers own disjoint blob hash ranges;
- part-manifest cleanup workers own disjoint `ManifestId` ranges or root namespaces;
- one coordinator owns registry fence, input seal, round visibility, global fence, and generation
  pointer advance;
- leases are work-dedup only.

Because root owner transitions carry both old and new manifest refs, target reducers do not need a
durable `RootEdgeIndex` to solve displacement. The displacement decision is made at the source root
shard, then the old/new manifest streams scatter blob deltas to target reducers.

Two replicas may process different blob target shards and part-manifest cleanup ranges concurrently.
No writer observes the GC's internal mapper/reducer products, and no generation is adopted, until the
`CompletionSeal` is written; the **retired-token view**, however, is published after the retire barrier
per `ViewableRound` (when all retired sets and part-manifest cleanup bundles for the round are durable),
which is exactly what writers consult for their publish gate.

## What Becomes Simpler {#what-becomes-simpler}

This redesign removes or shrinks several previously hard pieces:

- **No content-addressed tree revival race.** Byte-identical future manifests get different
  `ManifestId`s (a fresh `manifest_instance_id` in its root namespace), so stale work for old manifest `T1`
  cannot delete `T2` or apply `T1`'s blob decrements to `T2`.
- **No full-closure root journal.** Publish/drop records name old/new manifest refs only. The
  complete tree structure is stored once, in the part manifest object.
- **No recursive GC tree reads.** Fold reads one manifest object per old/new owner transition.
- **No nested static-folder ownership.** Directories are path prefixes inside the manifest, not objects
  in the protocol graph.
- **No GC-side cascade state.** Owner removal directly produces blob decrements and part-manifest cleanup
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
  transition. This design writes each part manifest once and writes compact owner transitions
  thereafter.
- **Debugging is more physical.** A manifest key shows its root namespace and build prefix, and its
  body explains every blob edge. A blob in-degree entry can point back to `(ManifestId, path)`.

The explicit tradeoff is also clear: tree dedup is gone. This is intentional. Tree objects are metadata
relative to blobs, and the storage saved by deduplicating tree metadata was buying a disproportionate
amount of GC protocol complexity.

## Safety Invariants {#safety-invariants}

The new `TLA+` model should be a branch of `CaIncarnationCore.tla` plus the useful precommit rules from
`CaBuildRootPrecommit.tla`.

Mandatory invariants and liveness obligations:

- `NoManifestIdReuse`: once a `ManifestId` appears in a visible root journal, no later action can bind
  that id to a different payload or owner lineage.
- `SingleManifestOwner`: each visible `ManifestId` has at most one structural owner.
  `PromotePrecommit` is an atomic owner move. Cross-table operations publish a fresh destination
  manifest; they do not share or move a manifest across namespaces.
- `CommittedManifestBodyRequired`: a committed owner transition cannot become visible unless the named
  manifest body exists and validates.
- `PrecommitMayReferenceMissingManifest`: a precommit owner may name a missing manifest body; such a
  precommit is a non-activating intent: it emits no blob edges and **cannot be promoted at all**; the
  writer must re-stage with a fresh `ManifestId` once the body is present.
- `RefMatchesBody`: the journal `ManifestRef` equals the `ref` inside the decoded manifest body; a
  read/fold against a mismatch fails closed.
- `ManifestNamespaceMatches`: a manifest body's `root_namespace_id` equals the owning root namespace;
  a mismatch fails closed (no cross-namespace ownership, no mis-scoped debris sweep).
- `MutablePayloadNotReachability`: mutable per-ref payload changes do not change `ManifestId`, do not
  emit owner transitions, and do not affect blob in-degree.
- `ManifestActivationMatchesEdges`: a non-activated (missing-body) precommit cannot be promoted, and
  promotion never adds edges. A precommit activation records whether the manifest body was readable;
  removals mirror only edges that were actually emitted; a promotion is a pure owner move on an
  already-activated manifest (Δ = 0), with no committed-add branch from a non-activated precommit.
- `CommittedNoMissingBlob`: every blob reachable from a committed root ref is present and not condemned
  in the committing writer's view.
- `PrecommitMayReferenceMissingBlob`: missing blob leaves under precommit are legal and not corruption.
- `BlobInDegreeMatchesActiveManifests`: durable blob target state equals the multiset of blob edges
  emitted by active manifests.
- `NoCommittedDangle`: a committed root ref never resolves to a missing manifest body or missing blob
  leaf.
- `NoReturn`: a deleted or overwritten blob token is never accepted as a future dependency.
- `ExactDeleteOnly`: destructive blob and manifest deletes require the exact observed token.
- `ViewableRound`: round `R` is writer-visible only after all retired sets and part-manifest cleanup
  bundles for `R` are durable.
- `JournalCoverage`: no root transition or part-manifest cleanup work is trimmed before its effect is
  included in a sealed generation or carried forward.
- `OrphanManifestDebrisDrains`: a manifest body staged before `PrecommitAdd` and never named by any
  live owner is eventually deleted after its writer prefix becomes conservatively sweep-eligible.

### Abstraction Boundary For The Model {#abstraction-boundary-for-the-model}

The model's safety identity is a **distinct** term, `ManifestSafetyId = (root_namespace_id,
manifest_instance_id)`, used **only** for the TLA+ abstraction (the `NoManifestIdReuse` /
`SingleManifestOwner` safety argument), where `manifest_instance_id` is random and never reused once it
becomes visible. The protocol identity, `ManifestId = (root_namespace_id, ManifestRef)`, is **not**
renamed: the protocol uses the full `ManifestRef` (`writer_instance_id`, `build_sequence`,
`manifest_instance_id`) to locate/address the object, while the safety argument rests on the unique
`manifest_instance_id` alone. `writer_instance_id` and `build_sequence` are **not** part of safety
identity: they are a **locator plus sweep-eligibility grouping** — the build prefix
`_manifests/<writer_instance_id>/<build_sequence>/`. The model therefore abstracts `ManifestId` to
`ManifestSafetyId`.

Because the orphan-sweep and key-collision proofs reason about the real key space, the chosen position
is to **model the build prefix explicitly**: the Phase-0 model represents a `BuildPrefixes` domain and a
manifest → prefix mapping with per-prefix sweep-eligibility, so the sweep deletes by prefix and the
collision proof covers the actual `_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>`
keys. The safety identity nonetheless remains `ManifestSafetyId = (root_namespace_id,
manifest_instance_id)`: writer and build are locator-only and never decide reachability, ownership, or
`NoManifestIdReuse`.

## Negative Controls {#negative-controls}

Each negative control must produce the expected counterexample before the phase is allowed to ship:

1. Reuse a `ManifestId` for a byte-identical future manifest: stale part-manifest cleanup work deletes
   the new manifest or applies old blob decrements to the new owner.
2. Allow two owners for one `ManifestId`, including by sharing a manifest across refs/namespaces:
   deleting one owner removes blob edges still needed by the other owner.
3. Make `PromotePrecommit` two separate CAS operations with a gap and no fail-closed retry: a false
   precommit reclaim can create a committed dangle.
4. Treat a missing manifest body under precommit as an activated manifest: later removal emits `-1`
   deltas for edges that were never added, causing undercount.
5. Let committed publish skip blob revalidation: a committed ref can name an absent blob.
6. Treat a precommitless speculative blob upload as protected by the future build: GC may delete it
   before `PrecommitAdd`, so promotion must revalidate/reupload rather than assume protection.
7. Omit the pre-precommit `_manifests` debris sweep: manifest bodies written before `PrecommitAdd` and
   then abandoned are never reachable from owner-transition fold and leak forever.
8. Delete build-scoped `_manifests` debris using a partial owner view or by wholesale deleting a dead
   build prefix: a live committed ref can lose its manifest body and become dangling.
9. Let orphan sweep use a frozen-seq / judged-dead heuristic as deletion authority: a still-running
   writer can lose its precommit manifest body. Safety survives only if the precommit becomes
   missing-body inactive and promotion fails closed; otherwise the model must find a counterexample.
10. Treat a missing manifest body under committed owner as an empty manifest: undercounts blob edges and
   permits over-delete.
11. Delete a manifest body before its owner-removal blob decrements are durable or carried forward:
   child blob references leak because the body needed for subtraction is gone.
12. Advance root cursor/token past unsealed owner-transition deltas: `SabotageCutOverclaim` shape.
13. Advance `gc/state.round` after only one target shard's retired set is durable: `ViewableRound`
   break.
14. Skip global fence for a racing publish: `INV_NO_DANGLE`.
15. Trim root journal below an unincorporated owner transition: `INV_JOURNAL_COVERAGE`.
16. Use non-exact delete or reuse blob tokens: existing `SabotageUncondDelete` and `SabotageReusedTag`
   shapes.
17. Carry only a bare `manifest_instance_id` (no `writer_instance_id`/`build_sequence`) instead of a full `ManifestRef`:
   the read path and GC fold cannot build the key, forcing an unsafe LIST/body scan.
18. Key source edges or cleanup work by `ManifestRef` alone instead of `ManifestId`: two root
   namespaces with the same ref merge unrelated blob edges or part-manifest cleanup work.
19. Decode a manifest whose body `root_namespace_id` differs from the owning namespace and accept it:
   cross-namespace dangle and a mis-scoped debris sweep (`ManifestNamespaceMatches` must reject it).
20. Follow a `ManifestRef` to an object whose body `ref` differs from it and accept it: the wrong
   manifest is folded or deleted (`RefMatchesBody` must reject it).
21. Put mutable per-ref payload into `PartManifestProto` or mint a new `ManifestId` for a
   mutable-only update: harmless metadata changes produce spurious blob deltas and can hide a real
   reachability transition in unrelated noise.
22. Promote a non-activated (missing-body) precommit, or let promotion add blob edges: must be rejected.
   A non-activated precommit is non-promotable, and promotion never adds edges — it is a pure owner
   move (Δ = 0) on an already-activated manifest. The old hazard (folding a promote as a pure owner
   move after the precommit was recorded missing-body, so a live committed ref loses its blobs) is
   eliminated by construction because the ordered journal activates before any promote and the
   committed-add-from-inactive-precommit branch no longer exists; the model must reject promotion of a
   non-activated precommit.
23. Advance the fold cursor past a live missing-body precommit (fold it as non-activated and move on):
   a later promotion emits Δ = 0 while no `+1` was ever emitted → the committed ref is under-protected
   (`INV_NO_DANGLE`). The fold barrier (not advancing the cursor past a missing-body precommit) must
   prevent this.

## Backpressure And Journal Encoding {#backpressure-and-journal-encoding}

Backpressure remains necessary, but the pressure points are smaller:

- root journal unfolded bytes;
- root journal encoded bytes;
- number of owner transitions since folded cursor;
- part manifest objects staged per build;
- `manifest_encoded_bytes` (total encoded size of one manifest);
- `manifest_entries` (entry count per manifest);
- `manifest_inline_bytes_total` and `largest_inline_entry_bytes` (inline payload is data, and the
  manifest is read on every part-open and every owner transition, so cap the total, not only per-file);
- `blob_delta_bytes_per_generation`;
- active precommit count and oldest precommit age.

The format boundary is part of the design:

- `Protobuf` is acceptable for control-plane records and envelopes: `gc/state`, registry records,
  the `FoldSeal` and `CompletionSeal`, root-journal headers/footers, small `RootOwnerEvent`s, and debug
  metadata.
- Length-prefixed per-record `Protobuf` is not acceptable for hot homogeneous data-plane streams at
  pool scale. That includes part manifest entries for very large manifests, blob delta runs,
  source-edge runs, blob in-degree runs, reducer inputs/outputs, and retired-candidate streams.

Hot data-plane streams use dense block-framed sorted binary runs:

```
RunFile
  RunHeader { magic, version, kind, key_schema, codec, block_size }
  DataBlock { block_len, record_count, min_key, max_key, checksum, payload }
  RunFooter { block_index, total_count, min_key, max_key, checksum }
```

`payload` is specialized by `kind` and sorted by the merge key, for example `blob_hash`,
`(blob_hash, source_id)`, or `(target_shard, blob_hash)`. Hashes are stored as fixed-width bytes,
counts and deltas are packed, source ids are dictionary- or run-length-encoded when useful, and
compression is optional per block. Hashes are high-entropy data, so compression must not be assumed to
save key bytes; the point of this format is to avoid per-record tags, varints, heap churn, and generic
parser dispatch at hundreds of millions of records.

The footer index is sparse. A reader can verify blocks independently, seek by key range when needed,
and merge streams with memory bounded by `O(number_of_inputs * block_size)`, not by pool size. S3
operation count is proportional to the number of run files and ranged block reads, not to the number of
records. Block size is a tunable threshold with a hard cap; the implementation should target large
sequential reads and writes while keeping per-block memory bounded.

`PartManifestProto` remains the logical manifest schema and may remain the envelope for manifest
metadata. Its `entries` payload follows the same data-plane rule for large manifests: block-framed
entry blocks, not one length-prefixed `Protobuf` message per entry.

When limits are exceeded, fail closed before publishing a root/precommit owner transition. Never
publish a committed owner transition that names a missing manifest body or whose blob leaves did not
pass revalidation. A precommit owner may name a missing manifest body only as a fail-closed intent
that protects no blobs and cannot promote until the body is present and validated.

## Phase Plan {#phase-plan}

### Phase 0 - Model And Format Skeleton {#phase-0-model-and-format-skeleton}

- Create `CaGcRootLocalPartManifestCore.tla`.
- Use `(root_namespace_id, manifest_instance_id)` as the unique safety identity, and **model the build
  prefix explicitly**: a `BuildPrefixes` domain plus a manifest → prefix mapping with per-prefix
  sweep-eligibility, so the orphan-sweep and key-collision proofs cover the real
  `_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>` key space while
  `writer_instance_id` / `build_sequence` stay locator-only (never part of safety identity).
- Model unique `ManifestId`s, structured manifest refs with `RefMatchesBody` /
  `ManifestNamespaceMatches` checks, strict single ownership, no manifest sharing across refs or
  namespaces, mutable ref payload as non-reachability state, precommit owner, committed owner, owner
  moves, missing manifest bodies and missing blobs under precommit, committed-body-required
  promotion, precommit activation state (activated vs. non-activated/non-promotable), pure-owner-move
  promotion with no blob delta, fail-closed promotion of a non-activated precommit, blob target
  in-degree, the always-global fence, recheck with the context-specific 404 policy, exact-token
  delete, part-manifest cleanup work, and pre-precommit orphan part-manifest cleanup.
- Add negative controls listed above.
- Define `ManifestRef`, `ManifestId`, `PartManifestProto`, the single ordered `RootOwnerEvent` stream,
  the write-once `FoldSeal` / `CompletionSeal` split, streaming root-journal control framing, and dense
  block-run framing for hot data-plane streams.
- Define reserved `_manifests` layout, `CasLayout::checkNamespace` rejection, and key validation.

No production behavior changes in this phase.

### Phase 1 - Part Manifests, Single GC Shard {#phase-1-part-manifests-single-gc-shard}

- Change `Build::stageTree` to mint root-local part `ManifestId`s instead of Merkle `TreeId`
  values.
- Stream-write part manifest objects under `_manifests`.
- Add writer best-effort cleanup and conservative GC backstop sweep for pre-precommit `_manifests`
  debris; do not use frozen-seq / judged-dead liveness as deletion authority for manifest bodies.
- Move precommit into the target root prefix.
- Add atomic `PromotePrecommit`.
- Replace `JournalRecord{Add, Remove}` reachability records with owner transitions carrying old/new
  `ManifestRef`s (not bare ids); add `ManifestRef`, `mutable_files`, and `published_at_ms` to the
  committed `RefRecord`.
- Change cross-table `republishRef`-style paths to publish a fresh destination manifest over the same
  blob hashes, then drop the source ref; `Atomic` database rename remains a CA no-op.
- Build a streaming blob in-degree generation from owner-transition manifest streams.
- Update `Store::resolveRef`, `Store::readTree`, and metadata read helpers to address root-local
  part manifests via the ref + `CasLayout`, enforce `RefMatchesBody` /
  `ManifestNamespaceMatches`, and serve path lookup from the manifest and optional directory index.
- Keep `gc_shards = 1`.
- Keep all-shard root scan and all-shard fence for the first behavior-changing implementation.
- Keep per-candidate `HEAD` in retire.

This phase removes global content-addressed tree objects, inline precommit closure records, resident
whole-pool `GcSnap` authority, expansion markers, nested tree objects, and cascade state.

### Phase 2 - Token-Diff Discovery And Lazy Read {#phase-2-token-diff-discovery-and-lazy-read}

- Add LIST token capability probes.
- Persist folded root shard state tokens with folded cursors.
- Skip unchanged root shards only when LIST token freshness is proved.
- Keep fail-closed fallback to body reads on ambiguous/missing tokens.

### Phase 3 - Lazy Trim {#phase-3-lazy-trim}

- Trim only below sealed generation coverage (`folded_cursor` in the `FoldSeal`), recording trim
  coverage in the `CompletionSeal`.
- There is **no lazy fence**: the fence is always global and is never skipped. Token-diff discovery
  (Phase 2) and trim are the only laziness in the protocol.

### Phase 4 - Target-Sharded Blob Reducers {#phase-4-target-sharded-blob-reducers}

- Enable `gc_shards > 1`.
- Root mappers scatter blob deltas by blob hash.
- Blob reducers merge disjoint target shards.
- Part-manifest cleanup workers process disjoint `ManifestId` ranges or namespaces.
- The global coordinator keeps the registry/fence/round-visibility responsibilities.

### Phase 5 - Retire-Token Optimization {#phase-5-retire-token-optimization}

Optional and separate. Remove per-candidate `HEAD` only after a model proves the stored token source is
complete and stale tokens can only cause under-delete, never over-delete.

## Operation Cost {#operation-cost}

| Quantity | Current code | Rev. 5 root-closure | Part manifests |
|---|---|---|---|
| Tree identity | content hash | content hash + `ref_epoch` | unique root-local `ManifestId` |
| Blob dedup | yes | yes | yes |
| Tree dedup | yes | yes | no |
| Publish journal size | tree hash | full transitive closure | old/new `ManifestRef`s |
| Tree structure storage | global `trees/` objects | global `trees/` object plus closure journal bytes | one root-local part manifest |
| Read-path tree cache sharing | content-hash sharing | content-hash sharing | per-instance cache; less sharing |
| GC memory | resident `O(pool)` snap | stream buffers | stream buffers |
| Tree-child GC state | `children_by_tree`, markers, cascade | none, but full closure in journal | none |
| Blob fold work | root journals plus tree expansion | closure-delta stream | changed owner manifests |
| GC tree-body reads | first expansion of content tree | none for root closures | one manifest read per old/new owner |
| Hot-path journal bytes | tree hash | full transitive closure | compact owner transition |
| Hot GC stream format | resident protobuf state | closure protobuf payloads | dense block-framed sorted binary runs |
| Pre-precommit tree debris | global tree debris/full GC | closure records avoid staged tree reads | namespace-scoped `_manifests` sweep |
| S3 GC scratch | snap generations, retired/outcomes | coarse generations | coarse blob generations and part-manifest cleanup bundles |
| Sharding blocker | target displacement in resident snap | solved by `RootEdgeIndex` | solved by root owner transitions |

Per changed owner transition, GC reads the affected part manifest and emits blob deltas. This is
`O(changed manifest entries)`, not `O(pool)`, and it avoids writing the same transitive closure into
every root journal event.

## Debuggability And Resume {#debuggability-and-resume}

Every durable generation must answer:

- root shards read/skipped/minted and why;
- `RootOwnerEvent`s folded;
- `ManifestId`s activated/deactivated;
- non-activated (missing-body) precommit intents and the fail-closed promotions they rejected;
- manifest bytes and entry count;
- blob deltas emitted per target shard;
- retired blob candidates;
- retired `ManifestId`s;
- orphan part-manifest cleanup namespace/build prefix;
- fence positions;
- recheck results;
- exact-token delete outcomes;
- trim cursors.

A single blob delete should be explainable as:

```
blob B
  last source edge: ManifestId(N, T), path P
  owner transition: ref R removed T at root version V
  folded in generation G
  retired in round Rn with token Tok
  fenced through root version F
  rechecked indeg 0
  deleteExact(B, Tok) -> Deleted
```

Resume rules:

- root-local part manifests are immutable; byte-identical existing writes may be adopted,
  divergent writes fail closed;
- generation phase seals are write-once and never mutated after their single write;
- generation-level resume keys off the phase seals: `completion_seal` present ⇒ the generation is done;
  else `fold_seal` present ⇒ resume at recheck; else redo fold;
- part-manifest cleanup work is keyed by `(round, generation, ManifestId)` and can be replayed;
- blob and manifest content deletes are exact-token and idempotent;
- orphan part-manifest cleanup outcomes are compact and replayable;
- root cursors advance only with sealed blob-delta coverage;
- round visibility advances only after all retired sets and part-manifest cleanup bundles are durable.

## Out Of Scope {#out-of-scope}

- No on-disk compatibility scaffolding for the old CA tree format.
- No content-addressed tree dedup.
- No nested root-local tree objects.
- No memoized closure object table.
- No full transitive closure in root journal records.
- No target-shard writes from the publish path.
- No removal of exact-token delete.
- No per-edge or per-candidate S3 object layout.
- No hot-path per-build state object solely to arbitrate orphan part-manifest cleanup versus
  `PrecommitAdd`.

## Open Questions {#open-questions}

1. Exact debug fields in `PartManifestProto` beyond the `ManifestRef`.
2. Whether `_manifests/<writer_instance_id>/<build_sequence>/` should be under the root namespace root directly or
   under a shard-specific subprefix tied to `root_shard`.
3. Internal part manifest indexing: block size, optional directory index, and path lookup layout.
4. How much manifest streaming work is acceptable in `PromotePrecommit` revalidation and whether to
   store a compact blob summary inside the same manifest object.
5. Exact control-plane `Protobuf` envelope and data-plane block-run details: block size, key schemas,
   per-block checksum, footer index, compression policy, and canonical entry-block encoding.
6. Exact conservative sweep-eligibility rule for `_manifests/<writer_instance_id>/<build_sequence>/`, including how
   `writer_instance_id` encodes the writer incarnation/epoch.
7. Exact backpressure thresholds for `manifest_entries`, `manifest_encoded_bytes`,
   `manifest_inline_bytes_total`, and `blob_delta_bytes_per_generation`.
8. How `fsck` should distinguish owner-visible missing manifest bodies from reclaimable
   pre-precommit part-manifest debris.
