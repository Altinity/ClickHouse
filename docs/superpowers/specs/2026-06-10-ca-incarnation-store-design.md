---
description: 'Authoritative design specification for the content-addressed (CA) MergeTree storage backend, incarnation-token core: one key per content hash, logical references in trees, backend-native exact-token deletes, CAS root manifests with embedded journal, two-tier GC (O(delta) fold + rare checkpoint-diff full GC), Keeper optional. Supersedes the 2026-06-07 EBR-era design.'
sidebar_label: 'CA incarnation store design spec'
sidebar_position: 1
slug: /superpowers/specs/ca-incarnation-store-design
title: 'Content-Addressed MergeTree Storage — Incarnation-Token Design Specification'
doc_type: 'guide'
---

# Content-Addressed MergeTree Storage — Incarnation-Token Design {#ca-incarnation-store-design}

**Status:** authoritative, single-source. **Supersedes** `2026-06-07-ca-merkle-store-design.md` (the
EBR/epoch/generation core) and the GC plan series s1–s4. The requirements contract
(`2026-06-08-ca-merkle-store-requirements.md`) remains in force **except** for the clauses explicitly amended in
§10. This document is written to be converted to a TLA+ model and model-checked (§12, appendix §A); the phased
refactoring of the current implementation onto this design is planned separately.

This is the TARGET design. Migration from the current implementation is out of scope here.

## Table of contents {#toc}
- [1. Overview — what changed and why](#overview)
- [2. Backend contract — incarnation tokens](#backend-contract)
- [3. Data model and object envelope](#data-model)
- [4. On-storage layout](#layout)
- [5. Writer protocol](#proto-write)
- [6. Reader protocol and FUSE readiness](#proto-read)
- [7. Regular GC — fold, retire, fence, recheck, delete](#proto-gc)
- [8. Full GC — checkpoint-diff, debris, leadership](#proto-fullgc)
- [9. Invariants, safety argument, failure handling](#invariants)
- [10. Requirements-contract amendments](#amendments)
- [11. Scale and cost](#scale)
- [12. Verification scope](#verification)
- [13. Open items and integration deltas](#open-items)
- [Appendix A. Formal model sketch (TLA+-convertible)](#appendix)

---

## 1. Overview — what changed and why {#overview}

The store remains **"Git for MergeTree": a Merkle DAG of immutable folders** on an object-storage disk
(`metadata_type = content_addressed`). Files are content-addressed blobs, folders (including every `MergeTree`
part) are content-addressed trees, and the only mutable objects are the **root manifests** — the GC roots and
commit points. What changed versus the 2026-06-07 design is the reclamation core. The EBR design put the
resurrection counter in the object **key** (`blobs/<H>/<g>`), which forced the `404→LIST` degraded read path,
`child_gen` inside tree identity (hence ancestor rebuilds when a floor moved), durable per-hash floors, and the
epoch/pin/quiescence machinery with Keeper required (old D1). This design moves the resurrection marker **into
the object** and the deletion precision **into the backend token**:

- **One key per content hash, forever.** `blobs/<H>`, `trees/<T>`, `packs/<P>`. Readers resolve logical names
  with single `GET`s; no generation in any key; no `404→LIST` path; trees reference **pure logical child
  hashes**, so identical folders hash identically regardless of resurrection history and a child's resurrection
  never rewrites ancestors.
- **Incarnations.** Every physical write of a key is an *incarnation*, identified by a backend-native
  **token** (`ETag`, GCS `generation`, …). All incarnations of a key are logically identical by a hard writer
  invariant (§5). A random in-body `incarnation_tag` forces every resurrection to produce a distinct body and
  therefore a distinct token even for identical logical content.
- **Deletes are exact-token only.** GC retire records carry `(kind, hash, observed_token, token_type)`; the
  physical delete is conditional on exactly that token. An incarnation that is unreachable at a post-fence
  recheck can never be referenced again (`INV-NO-RETURN`, §9), so arbitrarily delayed or duplicated deletes are
  harmless forever. This replaces generations, floors, epochs, pins, `safe_epoch`, `birth_epoch`/`commit_epoch`,
  and the quiesced-prefix reconcile.
- **Roots are CAS manifests with an embedded journal.** A publish is one conditional PUT that atomically
  updates the ref set and appends the reachability delta — the "delta source atomic with root update"
  requirement is satisfied by construction. The manifests are also the GC **fence** target.
- **Keeper is optional** (old D1 reversed). All coordination — GC lease, writer build heartbeats — has an
  S3-only binding; deletion safety never depends on leadership. A plain non-replicated `MergeTree` on a CA disk
  needs no Keeper at all. When Keeper is configured it is used opportunistically (faster election and liveness
  detection), with zero protocol divergence.
- **Two GC tiers.** Regular GC is an `O(delta)` in-degree fold over the journals with a
  retire → fence → recheck → exact-token-delete tail. Full GC is the rare checkpoint-diff mark that finds
  **debris** (objects no journal ever knew — crashed builds, gated by build heartbeats, never by writer clocks)
  and repairs **drift**. Both tiers share one deletion tail; there is no second deletion path.

Goals G1–G9 of the requirements contract are unchanged. The decisive liveness improvement over the EBR core: a
stuck or wedged writer **cannot stall reclamation of dropped parts at all** — there is no pin and no
`safe_epoch` for it to hold down; its only power is over its own unpublished debris (§9 liveness).

## 2. Backend contract — incarnation tokens {#backend-contract}

```text
Core rule: GC may delete only (logical_key, observed_incarnation_token).
           If the current incarnation differs, the delete must fail or affect only the old incarnation.
```

The protocol term is the **token**, never a specific backend mechanism. v1 operates in **current-object mode**:
one live object per key, and bucket versioning **never enabled** on the CA prefix. Suspended buckets are
rejected too: in a versioning-suspended bucket a delete inserts a null delete marker (and noncurrent versions
from a previously-enabled period linger invisibly), which breaks the one-live-object model just as a
versioning-enabled bucket does (there an `If-Match` delete "succeeds" by laying a delete marker while the bytes
leak invisibly). The probe checks this functionally and prefix-scoped — create and delete a probe key, fail
closed if the delete response reports a delete marker (`x-amz-delete-marker`) — so no bucket-level permissions
are required. A **versioned mode** (token = `versionId`/generation, delete by version) is a future, explicitly
separate feature requiring its own modeling of delete markers, noncurrent retention, and lifecycle interplay.

| Backend | Token | Binding | v1 status |
|---|---|---|---|
| AWS S3, versioning off | `ETag` | `PUT If-None-Match`/`If-Match`; `DeleteObject If-Match` (GA Sep 2025) | primary target, probe-gated |
| GCS | `generation` | `x-goog-if-generation-match` headers injected via the existing `SetAdditionalCustomHeaderValue` hook; generation read from response headers | binding specified, implementation deferred; fail-closed until probed |
| Azure Blob | `ETag` (write-sensitive) | `AccessConditions` on put/delete (SDK support in-tree); versioning/soft-delete must be off in v1 | supported, probe-gated |
| MinIO AIStor | `ETag` | documents `DeleteObject If-Match` | CI candidate, probe decides |
| MinIO OSS (archived 2026-02) | — | conditional PUT only; `If-Match` on DELETE is **silently ignored and the object is deleted anyway** (empirically confirmed 2026-06-11 on RELEASE.2024-09-13 and the final RELEASE.2025-09-07 — exactly the failure mode the enforced probe exists to catch) | fail-closed for v1 GC |
| Ceph RGW | — | no conditional delete documented | fail-closed for v1 GC |
| RustFS (1.0.0-beta.8) | `ETag` | **full conditional set empirically verified 2026-06-11**: DELETE `If-Match` enforced (wrong token → 412, object survives), `If-None-Match:*` create enforced, `If-Match` CAS PUT enforced — with both quoted AND unquoted ETags (the historical unquoted-strictness rustfs#1458 is closed and verified fixed in this image). Practice regardless: send tokens exactly as observed. Caveat: beta software | leading open-source CI candidate for GC e2e, probe-gated |

**Safety-critical primitives** (capability probe, extending the existing fail-closed `PoolCoordination` probe):
atomic whole-object PUT; **exact-token conditional delete, enforced** — a delete carrying a wrong token MUST
fail (this catches backends that silently ignore the header); a failed conditional write leaves the object
unmodified; CAS (`If-Match`) on the root-manifest objects; strong read-after-write and list-after-write;
versioning never enabled on the CA prefix (delete-marker probe above); and the **token-distinctness obligation**: the proof must establish that an
exact-token delete for an old incarnation cannot affect a newer current incarnation (the in-body
`incarnation_tag` plus body-sensitive tokens provide this on `ETag` backends; `generation` provides it natively).

**Hygiene-only primitives** (used by convention, not load-bearing): `PUT If-None-Match:*` for create (skips
re-upload on dedup hit), `PUT If-Match` for resurrect (lets a racing loser adopt the winner's incarnation),
`x-amz-copy-source-if-match` on server-side resurrect copies. Safety never rests on conditional PUT; the TLA+
model includes the unconditional-overwrite transition deliberately (§12).

## 3. Data model and object envelope {#data-model}

**Kinds.** *Blob* — one file's bytes. *Tree* — an immutable folder: canonically serialized, name-sorted entries.
*Pack* — one physical object carrying several small files' payloads. *Root manifest* — the mutable, CAS-updated
commit point (§4). A `MergeTree` part is a tree (`part_id ≡ T`); projections are subtrees; the per-part mutable
files (`txn_version.txt`, `metadata_version.txt`) live in the manifest's `RefPayload`, never in immutable
objects.

**Identity.** Blob hash `H` = `cityHash128` of the raw file bytes — taken from `checksums.txt` (D5 retained: no
re-read, no re-hash; fail-safe re-hash on attach-time mismatch). Tree hash `T` = `cityHash128` of the canonical
entries. Pack hash `P` = `cityHash128` over the pack index plus payload region. The hash algorithm is per-pool
policy (`hash_algo` in the envelope); single-trust-domain pools use `cityHash128`, adversarial/multi-tenant
pools use a collision-resistant hash per the requirements contract §11.

**Tree entries — three placements, one format:**

```text
entry = (name, placement, file_hash, file_size, placement_fields)
  placement = inline      → bytes embedded in the tree payload      (smallest files; part-open = 1 GET)
            | blob        → blobs/<file_hash>                        (large files; dedup pays here)
            | pack_slice  → (pack_hash, abs_offset, length)          (middle band; absolute offsets — one
                                                                      range-GET per read, no pack-index read)
  subtree entry = (name, subtree, child_tree_hash, tree_size)
```

Entries carry logical hashes and sizes only — **no tokens, no generations**. Every reference carries the size,
so no reader ever issues a `HEAD`. Packs are created per-part but referencing is free across parts (mutation
carry-forward may keep `pack_slice` entries into the source part's pack). Reclamation is pack-granular: GC
accounts one edge per distinct referenced pack; space trapped in mostly-dead packs is recovered by **repack**, a
normal writer operation. Packing policy targets object-store-efficient sizes; small underfilled packs are legal.

### 3.1 The CHCA object envelope {#envelope}

Every blob, tree, and pack object is `header ‖ [pack index] ‖ payload`:

```text
[0, 96)  CHCA core header, little-endian, fixed:
   char[4] magic="CHCA"   u8 format_version   u8 kind{blob,tree,pack}   u8 hash_algo   u8 flags
   u32 header_len (=96+ext in v1, ≥96, 8-aligned, ≤16 KiB)   u32 index_len (0 unless pack)
   u64 logical_size       = bytes covered by logical_hash = object_size − header_len
   u128 logical_hash      = hash over [header_len, EOF):
                            blob: raw file bytes · tree: canonical entries · pack: index ‖ payload region
   u128 domain_id         u128 incarnation_tag         u128 build_id
   u64 header_hash (CityHash64 over the 96 core-header bytes with this field zeroed)
[96, header_len)         TLV extensions {u16 type, u16 len, bytes}; unknown non-critical types skipped:
   0x0001 PROVENANCE (fixed): u64 created_at_ms, u128 creator_server_id, u32 ch_version,
                              u8 op{insert,merge,mutation,attach,repack,...}
   0x0002 INTENDED_REF (utf8, optional): "server/<table_uuid>/<part_name>"   — debris forensics
   padding: RAW ZERO BYTES from the end of the last TLV to header_len (NOT a length-prefixed
            type-0 TLV). The decoder stops TLV parsing at a zero type word and requires the
            remainder of […, header_len) to be all zero. header_len (and any fixed target it is
            padded to, e.g. the pool's blob header length) is 8-aligned.
[header_len, header_len+index_len)   pack index (deterministic encoding; packs only)
[header_len+index_len, EOF)          payload
```

`[0, header_len)` is the **incarnation zone**: excluded from `logical_hash`, it may differ between incarnations
of the same logical object; identity is everything below it. Key = hex(`logical_hash`) for all three kinds.
`header_len + logical_size == object_size` gives truncation detection for free.

**Checksum policy.** No payload bytes are ever hashed twice: at write the hash comes from `checksums.txt`; the
hot read path performs **no** CA-level payload verification — ClickHouse compressed blocks carry their own
per-block checksums verified on decompression. Payload-vs-hash verification exists only as the D5 attach-time
fallback and an explicit deep-scrub mode. Routine validation is the free structural set: magic, supported
version, kind ↔ key-prefix, key ↔ `logical_hash` (string compare), size arithmetic, `header_hash`
(`CityHash64` over the 96 core-header bytes with the field zeroed — the same hash family the rest of
ClickHouse uses for checksums; covers the header only, never content. The header is the one region
`logical_hash` does not cover; every load-bearing field is independently re-verified, so this is a
diagnostics-quality check, not a safety dependency — decision 2026-06-11, replacing the earlier CRC-32C).
Unknown critical extension (flag bit) ⇒ fail closed.

**Provenance is diagnostic only.** No protocol decision — retire, delete, debris classification, heartbeat
staleness — ever reads `created_at_ms` or any writer clock. Provenance serves humans, forensics, and the FUSE
`stat` synthesis (§6).

## 4. On-storage layout {#layout}

One pool = one disk root; one bucket/prefix = one pool.

```text
<pool>/
  blobs/<H[:2]>/<H>                        CHCA envelope, kind=blob
  trees/<T[:2]>/<T>                        CHCA envelope, kind=tree
  packs/<P[:2]>/<P>                        CHCA envelope, kind=pack
  roots/_registry                          NAMESPACE REGISTRY (mutable, CAS): {registry_version,
                                           fence_round, namespaces[]}. The authoritative namespace
                                           universe (decision 2026-06-12). A writer's FIRST publish
                                           into a namespace CAS-appends it here BEFORE creating any
                                           shard manifest; GC discovers namespaces FROM the registry
                                           (never LIST) and fences it like a shard. This orders
                                           namespace CREATION against the fence — without it, a
                                           first-publish-to-a-new-namespace is invisible to both
                                           fence horns and a stale-view writer could republish a
                                           condemned hash past the recheck (a dangle). Namespaces
                                           stay registered until full GC removes them with their
                                           final manifests (M-F).
  roots/<server_id>/<table_uuid>/<shard>   ROOT MANIFEST (mutable, CAS): the only commit point
  gc/state                                 CAS: lease{owner,seq}, round, snap_shards (GC constant),
                                           folded_cursor[root_shard], fence_version[round][root_shard],
                                           snap_generation, checkpoint_generation. (folded_cursor and
                                           fence_version are indexed by ROOT shard — they track per-root
                                           journal-fold progress and fence points; the snap/retired/outcome
                                           objects are indexed by target-hash-prefix snap_shard. Two distinct
                                           sharding axes: roots are the journal sources, snap shards the
                                           in-degree targets.)
  gc/snap/<gen>/<snap_shard>               in-degree snapshot: present-edge sets + tree expansion markers;
                                           generation-numbered, monotone, streaming-folded. SHARDED BY THE
                                           TARGET CONTENT-HASH PREFIX (snap_shard = hash_prefix(node_hash) %
                                           snap_shards; snap_shards a GC constant, default 1): every edge
                                           targeting a node lands in the NODE's own snap shard, so a node's
                                           in-degree is always INTRA-SHARD — no cross-shard aggregation, no
                                           intersection between shards. (decision 2026-06-11. NOT the root
                                           shard: root in-degree of a content object is global across roots
                                           and parent trees, which only a target-hash-prefix sharding keeps
                                           collision-free.)
  gc/retired/<round>.<fence_seq>/<snap_shard>  retire set: {kind, hash, observed_token, token_type, size};
                                           append-by-unique-path (two leaders never overwrite each other);
                                           sharded by the same target-hash prefix as the snap
  gc/outcomes/<round>.<fence_seq>/<snap_shard>  retire outcomes {deleted|absent|replaced|spared}; trimmed
                                           after checkpoint inclusion; same target-hash-prefix sharding
  gc/checkpoint/<n>                        full-GC checkpoints: reachable-set summary + cut_version[shard]
  builds/<build_id[:2]>/<build_id>         build heartbeat: {server_id, heartbeat_seq (monotone), provenance};
                                           keyed by build_id ALONE (a random u128, globally unique) so full GC
                                           resolves a debris candidate's heartbeat from the core header in one
                                           GET — heartbeat identity is protocol-visible, PROVENANCE stays
                                           diagnostic
  _pool_meta                               pool identity, format version, capability proof
```

**Root manifest** (`shard = hash(part_name) % N`, `N` fixed per table at creation; single writer — the owning
server; the only external CAS contention is the GC fence):

```text
manifest = { shard_version,                     // monotone, CAS-carried
             fence_round,                       // written only by GC; monotone
             refs { part_name → RefPayload{ T, tree_size, header bits, mutable per-part fields } },
             journal [ {+|-, part_name, T, shard_version_at_append} ... ] }   // trimmed ≤ folded_cursor
```

A publish is **one conditional PUT** updating `refs` and appending the journal record atomically. Detached parts
and `FREEZE`/`BACKUP` ref-sets are refs in their own namespaces under `roots/` (additional reachability roots,
table-lifetime-independent), exactly as in the superseded design.

**Encoding split (decision 2026-06-11).** Objects whose **bytes are identity** (hashed) are binary: the CHCA
envelope and the canonical tree payload (plus blob/pack payloads, trivially). Every **non-hashed metadata
object** — root manifests, `gc/state`, retired sets, build heartbeats, `_pool_meta`, checkpoints, outcome
logs — is **strict JSON**: a top-level object carrying `format` and `version` fields; parsing is fail-closed
(wrong `format`, unknown key, missing key, wrong type, malformed document ⇒ corruption error; newer `version`
⇒ not-implemented error). These objects are exactly the operational surface a human inspects with plain S3
tools during an incident; they are never hashed, so canonical byte stability is not required. Hashes and
tokens are JSON strings (lowercase hex / verbatim); counters and sizes are JSON numbers (bounded far below
2^53 in practice).

**Ownership.** Each root namespace has exactly one logical owner for ordinary publishes. Any transfer, attach
adoption, replica adoption, backup/freeze writer, or FUSE snapshot client either writes a **distinct** `roots/`
namespace of its own or performs an explicit CAS ownership handoff. Cross-owner concurrent publishes to the
same root shard are outside the v1 protocol except through that handoff. `DROP TABLE` rewrites each shard to a
tombstone (`refs:{}`, journal of `-` records); GC deletes the manifest object itself only after
`folded_cursor[shard]` reaches its final `shard_version` **and** a durable checkpoint records it. Bounds are
explicit: max manifest size (size guard + alert), journal trimmed only under `INV-JOURNAL-COVERAGE` (§9), `N`
sized so the refs map stays small (online resharding is out of v1).

**Keeper (optional, when configured):** leader election via ephemeral-sequential node instead of lease polling;
per-build ephemerals instead of heartbeat objects. `O(active writers)`, nothing durable, zero protocol
divergence; total Keeper loss degrades to the S3-only binding with no durable impact.

## 5. Writer protocol {#proto-write}

Hard rules first — these are the writer-side invariants the model checks:

```text
W-SAME-CONTENT  a writer may PUT key K only with a body verifying logical_hash = K in K's namespace
                (kind, hash_algo, domain match). All incarnations of a key are logically identical.
W-FRESH-TAG     every non-retry upload attempt uses a fresh random incarnation_tag (a retry of the SAME
                attempt may reuse body+tag — same incarnation, harmless). This is what makes token
                recurrence impossible on body-sensitive-token backends.
W-HEARTBEAT     the build heartbeat is durable before the first object PUT and renewed in background.
W-DEP-SET       the writer maintains publish_dependency_set = {(kind, hash, token | live-root-evidence)}
                covering EVERY object the publish makes reachable — own uploads and trees included.
W-EVIDENCE      a tokenless (live-root-evidence) member is publishable iff its hash has NO entry in the
                writer's retire view AND the evidence is as fresh as the view (recorded at a view round
                >= the current one). Any hit forces resolution to a token-bearing entry: HEAD the key,
                then adopt the current token if it is not condemned, else resurrect. An unresolved
                tokenless member whose hash matches any condemned entry may not be published.
                STALE evidence with NO hit (amended 2026-06-12, implementation-discovered): because
                retire entries drop on confirmed outcomes (F1), "no entry" can mean
                condemned-deleted-and-dropped — the durable witness is the OBJECT. On a view refresh,
                every tokenless member recorded under an older view round is RE-OBSERVED (one HEAD):
                present ⇒ resolve to the current token (adopt or resurrect); absent ⇒ re-create or
                abort retryably — never publish a dangling reference. The "no source-root
                revalidation" economy holds only within one fence/recheck round window.
                SCOPE (amended 2026-06-12, model-discovered): tokenless evidence is permitted ONLY
                for members the writer WITNESSED as pinned — children carried forward from a source
                tree the writer actually read via a live root (the read is the observation; the live
                source pins them for the round window). Whole-object adoption of a root that nothing
                is known to pin (FREEZE, detached re-attach, replication relink) is a COLD REUSE,
                never blind evidence: one HEAD at adopt time, recording a token-bearing member
                (absent ⇒ fail closed; condemned ⇒ resurrect). Blind root evidence is a dangle: a
                detached tree reclaimed by a COMPLETED round has no view hit (entries dropped on
                outcomes), and a view already at the current round triggers neither the re-observation
                above nor a fence refresh — nothing would catch the dead reference.
W-PUBLISH-GATE  the publish CAS is valid only if: retire_view_round ≥ manifest.fence_round, AND no
                dependency-set member is condemned in that view, AND the writer's own heartbeat renewal is
                recent by its own clock (local sanity bound — liveness, never the safety argument).
W-REVALIDATE    a token observation is valid only relative to the retire view at which it was made (F1,
                model-discovered). Retire entries drop on confirmed outcomes, so the retire view alone
                cannot condemn a stale observation — the durable witness is the object itself. Whenever
                the writer refreshes its retire view (a fence-advanced CAS conflict), every token-bearing
                member whose hash has NO entry in the refreshed view must be RE-OBSERVED (one HEAD):
                current token == observed ⇒ keep — safe by the IN-FLIGHT DISJUNCTION (model-checked): a
                delete in flight for (h, t) implies its retire entry is still held OR t was already
                displaced (current ≠ t, forced through a gate-mandated resurrect). A delete for the
                CURRENT token therefore implies a held entry (displacement is excluded), which would be
                a view hit — so no-hit + current==observed is publishable. Current token differs ⇒ treat
                as a fresh cold reuse of the current token (adopt or resurrect); key absent ⇒ re-create.
                Cost: HEADs only on fence-conflicted retries, only for stale-observed members — rare by
                construction.
W-TREE-BUILD    bottom-up build discipline, at every level (F2, model-discovered): a tree object is
                created only after all of its children exist (its hash commits to them), and a tree ref
                may be published only when every object in the new tree's transitive closure is present
                and not condemned in the writer's view — W-DEP-SET applied to the closure, stated
                explicitly for trees. The model checks the one-level case; the transitive nested-subtree
                case is a recorded residual.
W-MANIFEST-CAS  root-manifest writes are always CAS (refs+journal are not idempotent content).
W-REGISTER      before the FIRST publish into a root namespace, the writer CAS-appends the namespace
                to roots/_registry; the registry's fence_round acts as a gate floor for that publish
                (view round < registry fence_round ⇒ refresh + W-REVALIDATE before proceeding), and
                the manifest gate then uses max(manifest.fence_round, registry fence at registration).
                Already-registered namespaces skip this (the per-shard fence carries the ordering:
                R3 fences ALL root_shards shards of every registered namespace each round, minting
                fence-only manifests for absent shards — the create-if-absent race against a first
                shard publish is the required total order). (decision 2026-06-12)
```

Publishing a part `name`:

1. **Heartbeat** durable (`builds/<build_id>`, `heartbeat_seq` monotone, `server_id` in the body), then build
   locally; hashes from `checksums.txt` (D5).
2. **Upload by placement** (inline → tree payload; middle band → per-part packs; large → standalone blobs),
   recording a dependency-set entry per object:
   - **New content:** `PUT If-None-Match:*` directly (default), or HEAD-first for large blobs where a rejected
     conditional PUT would still ship the body (`100-continue` is an implementation concern). Token from the
     PUT response.
   - **Cold reuse** (key exists): the existence check yields the current token; check the retire view —
     condemned ⇒ **resurrect**, else reuse as-is, free.
   - **Resurrect:** fresh `incarnation_tag`; bytes in hand ⇒ re-PUT (`If-Match` hygiene); no bytes ⇒
     server-side multipart-copy-to-self with a changed part split (`copy-source-if-match` guards the source;
     the body is byte-identical, the token still changes) or GET+PUT for small objects. A same-logical-content
     resurrection never changes parent hashes.
   - **Carry-forward / fetch-by-reference:** no requests — evidence is the live source root; check the retire
     view by hash and escalate to a HEAD only on a hit (rare: the retire set is bounded by reclaim backlog).
     No source-root revalidation is needed at publish: the fence/recheck handshake covers both race horns
     (§7 argument, step 3/4).
3. **Trees** bottom-up; reused trees are dependency-set entries like any cold reuse (a just-dropped identical
   part's tree can genuinely be condemned mid-build).
4. **Publish = one CAS** under `W-PUBLISH-GATE`: set `refs[name]`, append `{+, name, T}`, `shard_version++`.
5. **On CAS conflict:** re-read. `fence_round` advanced ⇒ refresh the retire view to ≥ that round and
   re-validate the **whole** dependency set under `W-REVALIDATE`: members with a view hit ⇒ resurrect; members
   whose hash has no entry but whose token was observed under the old view ⇒ re-observe (HEAD) and keep / adopt
   / re-create per the rule; then retry. Own-thread race ⇒ plain retry.
6. **Drop** = the same CAS shape: remove `refs[name]`, append `{-, name, T}` — ref removal and delta record
   atomic by construction.
7. **Crash anywhere before publish** ⇒ uploaded objects are debris (attributed by `build_id`/`INTENDED_REF`),
   reclaimed by full GC after heartbeat expiry. Abort = stop (optionally delete own heartbeat). Nothing
   dangles: no root ever named the new objects.

The wedged-heartbeat trace (why the gate, not the heartbeat, is the safety): a writer whose heartbeat thread
stalls while its commit path lives may find its own uploads condemned by full GC. Its publish either conflicts
with the fence (⇒ refresh ⇒ sees its uploads condemned ⇒ resurrects them — it has the bytes, it built them) or
landed pre-fence (⇒ the recheck folds its journal record ⇒ in-degree > 0 ⇒ spared). Either way: self-healing,
no dangle, no clock in the argument.

Incremental GC additionally honors a per-server build watermark (see
`docs/superpowers/specs/2026-06-16-ca-build-watermark-design.md`) as a co-liveness mechanism for in-flight
builds — it spares an `everEdged ∧ InDeg=0` blob whose owning build is still active; the publish gate remains
the safety backstop.

## 6. Reader protocol and FUSE readiness {#proto-read}

Reading: resolve the part via the server's in-memory state (manifests are read at attach/startup, parts cached
as today) → `GET` tree (or cached) → ranged `GET`s per entry placement, all bounded by sizes carried in the
references; payload offsets are `header_len`-shifted constants. Readers have **no GC awareness** — no retire
sets, no fences, no tokens. On a `404`: re-resolve the ref; ref gone ⇒ the part was dropped (clean failure);
ref still names the missing key ⇒ raise a storage exception — that surfaces an `INV-NO-DANGLE` violation
instead of hiding it. A present-but-condemned object still reads correctly (retirement blocks reuse, never
reads).

**FUSE readiness** (a later phase, designed-for now): a read-only FUSE client is plain S3 GETs — root manifests
→ refs as directories → trees as directories → entries as files; no Keeper session (Keeper-optional is what
makes a self-contained FUSE client possible); no HEADs (sizes everywhere); immutable one-key-per-hash objects
make a local cache trivially correct with infinite TTL; `PROVENANCE.created_at_ms` synthesizes `mtime`.
`readdir` of a table merges its `N` shard manifests. Drop races surface as `ESTALE`/`ENOENT` after re-resolve;
POSIX open-after-unlink semantics are explicitly not promised in v1. A FUSE client needing a stable snapshot
**publishes a ref in its own `roots/<client_id>/` namespace** — a first-class GC root under heartbeat
discipline; zero new protocol. A writing FUSE later = implement §5, which is deliberately client-agnostic.

## 7. Regular GC — fold, retire, fence, recheck, delete {#proto-gc}

Leader-paced via the `gc/state` lease — **work dedup only**; every step below remains safe under split-brain. A
stale leader may duplicate work, write redundant retire entries (unique paths), or issue exact-token deletes; it
can never roll back monotone state (`folded_cursor`, `fence_round`, generations only increase; fence versions
and cut vectors immutable once recorded; the recheck folds from durable `gc/state` cursors + journals, never
from leader memory).

**Edge identities** (in-degree = count of distinct **present** edges; set semantics, last-op-wins per edge in
shard order — duplicates can never double-count; **GC state may overcount, must never undercount**):

```text
root edge:  (root_shard, part_name, T)
tree edge:  (parent_T, child_kind, child_hash)        — one per distinct child
pack edge:  (T, pack_hash)                            — one per distinct referenced pack, not per slice
```

**Round R:**

```text
R1 FOLD     per root shard: stream-merge journal records in (folded_cursor, shard_version] into the snap
            shards (new generation, durable BEFORE cursors advance by gc/state CAS). On the first '+' to a
            tree T with no expansion marker: read T once, add its child-edge set, set marker(T)
            (marker(T) ⇔ T's child edges present). Candidates = journal-known nodes whose in-degree
            transitioned to 0. Fresh uploads are invisible here by construction — never candidates.
R2 RETIRE   per candidate: one HEAD observes the current token; append {kind, hash, observed_token,
            token_type, size} durably to gc/retired/<R>.<fence_seq>/<shard>. Retired ≠ dead: it is the
            writer-facing "resurrect, don't reuse" barrier.
R3 FENCE    CAS the namespace registry (fence_round := R, monotone), then CAS EVERY root shard of
            EVERY namespace in the registry AS DECODED BY THE COMMITTED REGISTRY-FENCE CAS — the
            FENCE-TIME universe, never the fold-time one. The registry is CAS-append-only, so
            fence-time ⊇ fold-time; a namespace registered between the fold's registry read and
            the registry-fence CAS would otherwise fall between the two horns (below the registry
            fence ⇒ its writer observes no floor; absent from the fold-time universe ⇒ its shards
            never fenced or recheck-folded — a dangle window; found and fixed 2026-06-12). Per
            shard: fence_round := R (monotone max), MINTING a fence-only manifest
            (create-if-absent CAS) for absent shards — the create race against a first
            publish is the required total order; record fence_version[R][shard] (and the registry's
            version) in gc/state. One fence covers the whole round's candidate set. Discovery (R1)
            reads the registry, never LIST. (amended 2026-06-12)
R4 RECHECK  fold every shard through ≥ its recorded fence version (provable, not asserted). Per entry:
   +DELETE    in-degree > 0            → outcome=spared, drop entry
              else DELETE If-Match tok → 2xx: outcome=deleted (trees: cascade step below), drop entry
                                       → 404: outcome=absent (trees: ensure cascade ran), drop entry
                                       → 412: outcome=replaced (a resurrection won — count the save), drop
            Dropping entries is safe for the DELETE side in every case: token-exact deletes cannot affect
            later incarnations. Note the spared branch CAN orphan an in-flight delete (send, then a
            gate-forced resurrect + publish raises in-degree, then the recheck spares and drops the
            entry) — safe, because the orphan's token was displaced by that resurrect first. The precise,
            model-checked guarantee is the IN-FLIGHT DISJUNCTION: a delete in flight implies its entry is
            held OR its token is already displaced (≠ current). The PUBLISH side does not rest on entry
            retention: a stale token observation is made safe by W-REVALIDATE (§5), not by finding an entry.
```

**Cascade is a pipeline step, not a foldable record** (this closes the cascade-vs-recreate race): round R's
snap update applies, in strict order, (1) journal folds up to R's fence versions — expanding trees on first
`+` iff unmarked — then (2) for every round-R entry with outcome `deleted` (or `404` against a held entry,
which proves our own crashed delete landed): strip that tree's child edges and clear its marker; only then may
any later round fold records beyond R's fence versions. A re-create-and-publish racing the deletion necessarily
lands at a `shard_version` above R's fence version, so its re-expansion folds **after** the strip by
construction. Replay of a crashed round re-runs both steps from the durable outcome log over set semantics —
no-ops. Cascade never runs on `412` (the new incarnation is present and keeps its children pinned; its own
lifecycle handles them). All incarnations of a tree are payload-identical, so reading the current incarnation
for the child list cannot diverge from the retired one.

**Why no delete can ever be wrong (the no-return argument):**

1. A delete for `(hash, token)` is issued only after: the retire entry is durable; every root shard is fenced
   at recorded versions; a fold through those versions shows in-degree 0.
2. Any publish making `hash` reachable is a manifest CAS, totally ordered against the fence CAS on its shard.
3. Publish **before** fence ⇒ its journal record sits below the fence version ⇒ the recheck folds it ⇒
   in-degree > 0 ⇒ spared, no delete.
4. Publish **after** fence ⇒ the writer saw `fence_round ≥ R` ⇒ its retire view includes round R ⇒ if the
   condemned token's entry is still held, the dependency-set gate finds it ⇒ resurrect (new token,
   `W-FRESH-TAG`) or abort. If the entry already dropped on a confirmed outcome, `W-REVALIDATE` re-observes:
   deleted/absent ⇒ re-create; replaced ⇒ adopt the newer token or resurrect; spared ⇒ the object is live and
   uncondemned at its current token (publishable — a delete in flight for the CURRENT token would require a
   held entry by the IN-FLIGHT DISJUNCTION, since displacement is excluded for the current token; a held
   entry would be a view hit). In every branch, nothing ever again depends on the retired incarnation.
5. Token distinctness (probed capability + `W-FRESH-TAG`) ⇒ the deleted incarnation can never again be
   current ⇒ any delayed, duplicated, or zombie-leader delete is forever harmless — it 412s or hits exactly
   the already-condemned incarnation.

## 8. Full GC — checkpoint-diff, debris, leadership {#proto-fullgc}

The rare tier, for the two things regular GC structurally cannot see: **debris** (objects no journal ever
knew) and **drift** (snap diverging from truth). One lease serializes both tiers (hygiene, not safety).

**Coherent cut.** Each manifest read is atomic and carries `shard_version`; the walk records
`cut_version[shard] :=` the version actually read, incorporates reachability and journal state **exactly**
through those versions, sets folded cursors to exactly those values, and stamps the checkpoint with the
immutable cut vector. **Claimed authority never exceeds incorporated state** — that is the only thing a torn
multi-shard walk could get wrong: cross-shard "no real instant" is the same per-shard-prefix consistency model
the incremental fold always has, and a missed publish is re-seen by the deletion tail's own fence/recheck.
Checkpoint publication is a `gc/state` CAS that fails if any cursor already advanced past its cut; the gap is
then folded forward into the rebuilt snap (a delta) and the CAS retried. The previous checkpoint stays
authoritative until that CAS lands.

**Walk** (range-sharded; per-range resumable state: prefix bounds, continuation token, output generation):
LIST `roots/` + read manifests (the authority) → stream tree reachability → sorted-stream join against LIST of
`blobs/`, `trees/`, `packs/` — `O(1)` memory. Three classes:
- **referenced-and-present** → rebuild snap shards authoritatively (new monotone generation);
- **referenced-but-absent** → re-read the ref's shard at current version (a concurrent legitimate drop is not
  an incident); if still referenced ⇒ loud `INV-NO-DANGLE` alert — the loss detector, never auto-repaired
  silently;
- **present-but-unreferenced** → debris candidates.

**Debris classification — no writer clocks, ever.** Per candidate (rare): range-GET `[0, header_len)` →
`build_id` (+ `INTENDED_REF` forensics) → one GET of `builds/<build_id>` (heartbeats are keyed by `build_id`
alone, §4 — the core header is sufficient, no diagnostic TLV is consulted). The build is **alive** iff its
heartbeat object exists and its
monotone `heartbeat_seq` (and token) changed across the GC's *own* observation window — GC-observed staleness,
single clock, never `mtime`. Alive ⇒ skip. Unreadable/ambiguous/backend error ⇒ skip, fail closed. Dead ⇒ the
candidates enter **the same retire → fence → recheck → exact-token-delete tail as regular GC** — no second
deletion path. Even a misjudged "dead" is safe: the writer's eventual publish hits the fence/retire-view gate
and resurrects (§5).

**Leadership, S3-only baseline.** `gc/state.lease` is CAS-stolen after observed non-renewal across the
contender's own waiting window. With Keeper configured, ephemeral election and per-build ephemerals replace
lease-polling and heartbeat objects — latency/cost only.

## 9. Invariants, safety argument, failure handling {#invariants}

- **`INV-NO-DANGLE`** — a live ref's transitive closure resolves through present keys. Readers never
  substitute; a ref naming an absent key is a surfaced storage exception.
- **`INV-NO-LOSS`** — a physical delete requires: durable retire entry; all-root-shard fence at recorded
  versions; fold-through-fence recheck showing in-degree 0; exact observed token; monotone `gc/state`.
- **`INV-NO-RETURN`** (replaces `INV-NO-ABA`) — once incarnation `(kind, hash, token_old)` is retired and
  passes the post-fence zero-reachability recheck, **that exact token** can never again be a valid dependency
  of any publish. The logical key may become current again — only as a **different** token, which a stale
  delete for `token_old` cannot affect. One-key-per-hash is the design; the key returning is normal.
- **`INV-OVER-COUNT-ONLY`** — every failure (lost fold, crashed leader, stale snap, duplicated records,
  misjudged heartbeat) delays reclamation; none accelerates a delete past the gates.
- **`INV-S3-COMPLETE`** (strengthened) — S3 alone is both the durable truth and sufficient coordination; Keeper
  is an accelerator. Total Keeper loss loses nothing and blocks nothing.
- **`INV-MONOTONE-GC`** — cursors, `fence_round`, snap/checkpoint generations only increase; fence versions and
  cut vectors are immutable once recorded; retire/outcome logs are append-by-unique-path.
- **`INV-JOURNAL-COVERAGE`** — for every shard, the manifest journal contains every record in
  `(folded_cursor, shard_version]`, or a durable checkpoint/snapshot generation included by `gc/state` proves
  those records already incorporated. A manifest CAS may trim records only after the corresponding folded-cursor
  advance is durable. ("Compact the manifest for size" is never a reason to trim.)
- **`W-SAME-CONTENT` / `W-FRESH-TAG`** — writer-side invariants making unconditional overwrite benign and token
  recurrence impossible (§5).

Liveness:
- **`LIVE-RECLAIM`** — every journal-known node that becomes permanently unreachable is retired and deleted
  within ~2 regular rounds; debris is reclaimed by the first full GC after its build's heartbeat expiry.
- **`LIVE-BOUNDED-WRITER-IMPACT`** — a writer cannot stall regular reclamation of journal-known dropped roots
  **at all** (no pins, no `safe_epoch`); it can delay only debris attributed to its own `build_id`, until
  heartbeat expiry and full-GC classification.
- **`LIVE-COMMIT-PROGRESS`** — single-writer shards make CAS conflicts local: own threads (serialized locally)
  and the GC fence (bounded by round cadence).

| Fault | Outcome |
|---|---|
| writer crash mid-build / mid-upload | debris under its `build_id`; heartbeat-gated full-GC reclaim; nothing published, nothing dangles |
| wedged heartbeat, live commit path | publish hits fence/retire-view gate ⇒ resurrect own condemned uploads ⇒ self-healing (§5 trace) |
| publish-CAS conflict storms | bounded: own-thread races local; fence conflicts once per GC round |
| GC leader crash at any step | idempotent replay from durable round state (journals, retire+outcome logs, monotone snap generations) |
| split-brain leaders | duplicate work only; monotone state + unique paths + token-exact deletes |
| delete delayed arbitrarily (zombie) | 412 or hits the already-condemned incarnation (`INV-NO-RETURN`) |
| Keeper total loss | nothing: S3-only is the baseline; Keeper-mode degrades to it |
| backend lacks exact-token delete / versioning on | capability probe fails ⇒ `metadata_type = content_addressed` refused, fail-closed |
| torn manifest write | impossible by environment contract: a failed conditional write leaves the object unmodified |
| full-GC misclassification (any) | the shared deletion tail re-verifies against current journals before any delete |
| lost/torn snap or checkpoint | previous generation stays authoritative; rebuild via full GC; over-protective |

## 10. Requirements-contract amendments {#amendments}

This design **deliberately replaces** the following clauses of `2026-06-08-ca-merkle-store-requirements.md`;
everything not listed stands unchanged (G1–G9, the two-tier GC shape, `O(delta)` regular GC, packs,
mutable-files, observability, probe scope — the probe scope is *extended*, §2).

1. *"A ref or tree must name the exact `(hash, locator)` it reads"* → **replaced.** Refs and trees name
   logical identity only; physical exactness lives solely in GC retire records `(hash, observed_token)`.
   Sound because all incarnations of a key are logically identical (`W-SAME-CONTENT`) and deletes are
   token-exact (`INV-NO-RETURN`).
2. *"Preferred v1 baseline: locators are unique and never reused; a deleted physical key is never recreated"*
   → **replaced** by one-key-per-hash with incarnation tokens: key reuse is normal, token recurrence is
   forbidden (probed capability + `W-FRESH-TAG`).
3. *Build-root discipline (durable build-roots before locator creation/reuse; GC treats build-roots as
   reachability roots)* → **replaced** by: regular-GC candidates restricted to journal-known nodes (fresh
   uploads structurally invisible); the publish dependency-set gate against the fenced retire view; and
   heartbeat-gated debris reclaim in full GC. The goal — no timing-only protection for in-flight builds — is
   preserved; the trailing clause "time-based retention applies only to debris" is narrowed further: debris
   reclaim is gated by GC-observed heartbeat staleness, not wall-clock object age.
4. *Retire-marker cleanup rules* → **superseded** by token-exactness: entries drop on
   deleted/absent/replaced/spared; outcomes logged until checkpoint inclusion.
5. *Environmental assumptions* → **extended**: exact-token conditional delete (enforced) and
   versioning-never-enabled-on-prefix (delete-marker probe) are required; conditional PUT is demoted to hygiene.
6. *Build-root expiry/fencing baseline (heartbeat + GC expiry)* → retained in spirit; the mechanism is the
   monotone `heartbeat_seq` judged by GC-observed windows.
7. *Old D1 (Keeper required)* → **reversed**: Keeper optional; S3-only is the fully-supported baseline.
8. *"Visible roots must be fenceable as a set or shard"* → now **satisfied directly** (CAS shard manifests +
   `fence_round`), no longer worked around via epochs.

## 11. Scale and cost {#scale}

Per the S3 ops cost model (write-tier ≈ 12.5× read-tier; `DELETE` treated as free **in this reference cost
model** — a backend that charges deletes or prices `LIST` differently must restate the budget, as the
requirements contract already demands):

- **Commit:** `F` blob/pack PUTs (the unavoidable bytes) + 1 tree PUT + **1 manifest CAS PUT** — the journal
  rides inside it; no separate delta-log write, no per-object metadata ops, no Keeper ops.
- **Regular GC round:** journal GETs + `O(new parts)` tree GETs + touched snap-shard PUTs + `O(candidates)`
  HEADs + `#shards` fence PUTs (one fence per round, candidate-count-independent) + free deletes. No LIST.
- **Full GC:** LIST-dominated (~10⁸ paginated calls at the 10¹¹-object stress point); range-sharded, rate-
  limited, resumable; one range-GET per debris candidate. Cadence days-to-weeks or metric-triggered.
- **Reads:** one GET per object; part-open = 1 tree GET (inline placements); packed reads = one range-GET.
  Never a HEAD, never a LIST, never a `404→LIST`.
- **Resurrection** (rare — only dedup-vs-retire races): server-side copy or small re-PUT.

Observability (extends the contract's list): `fence_round`, folded cursors and journal backlog per shard,
retire-set size/age and **412-saved-by-token counter** (a first-class health signal), outcome-log stats, debris
estimate and oldest dead-build age, heartbeat ages, snap/checkpoint generations and full-GC progress,
`INV-NO-DANGLE` alerts, per-tier billable-request counters, manifest size guard.

## 12. Verification scope {#verification}

A **new TLA+ model** replaces `CaGcCore.tla` (which encodes epochs/pins/floors and is invalidated wholesale).

State: keys × tokens (naturals); manifests `(shard_version, fence_round, refs, journal)`; snap as present-edge
sets + expansion markers + per-shard cursors; retire entries + outcomes; heartbeats (monotone seq); **in-flight
delete messages that may land arbitrarily late** (first-class). Actions: writer create/reuse/resurrect/
publish-CAS/drop/crash — including the **unconditional-overwrite transition** (deliberately modeled: safety
must not rest on PUT conditions) and the wedged-heartbeat zombie publish; GC fold/retire/fence/recheck/delete/
cascade-pipeline/full-walk/checkpoint-CAS; split leaders; Keeper wipe (no-op on durable state).

Must-check scenarios:
- both fence horns: publish-before-fence is spared by the recheck; publish-after-fence resurrects;
- **retired-old-token vs newer-current-token** (the heart of the design): with `(H, tok_old)` retired while
  `H` is current at `tok_new`, a writer may publish a dependency on `tok_new`, may never publish one on
  `tok_old`, and the old-token delete cannot remove the `tok_new` object;
- zombie delete landing after resurrect (412) and after re-reference of a spared entry;
- cascade-vs-recreate: the pipeline ordering — strip strictly before folding any post-fence-version record;
  replay of a crashed round is a no-op;
- full-GC cut: claimed authority never exceeds incorporated state; checkpoint CAS fails if cursors moved;
- debris: dead build reclaimed (no leak); live build's edge-less uploads never deleted (no loss);
  misjudged-dead build self-heals at publish;
- drop/re-attach same name and same `T` (edge set semantics under `-`/`+` replay).

Invariants: `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_RETURN`, `INV_OVER_COUNT_ONLY`, `INV_MONOTONE_GC`; temporal:
no-leak-forever under fairness. The model states its environment assumptions explicitly (atomic conditional
ops, token distinctness as a parameter) and must not assume more than §2 requires.

### Model status and findings {#model-status}

The model is implemented and model-checked: `docs/superpowers/models/CaIncarnationCore.tla` (run guide
`CaIncarnationCore_README.md`; per-stage results, bounds, and residuals `CaIncarnationCore_RESULTS.md`). Every
must-check above is covered by a staged config, and each load-bearing rule has a **sabotage negative control**
that must — and does — produce a counterexample (`sab_nofence`, `sab_norecheckfold`, `sab_noretireview`,
`sab_unconddelete`, `sab_reusedtag`, `sab_cascade`, `sab_cutoverclaim`). Bounds are reduced per stage to fit the
state space (stage 2 one hash; stage 3 single writer; stage 4 split into a `MaxLog=2` debris + two-shard-cut
config and a `MaxLog=3` journaled-delete + full-GC-tree config; stage 5 `MaxLog=4`); the reductions and what each
drops are recorded in `RESULTS`.

**Checked invariants:** `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, and
`MonotoneGC` (= `INV-MONOTONE-GC`, an action property). `INV-OVER-COUNT-ONLY` is **not** a separate checked state
invariant — it holds by construction (edge-set fold; "may overcount, never undercount"), and its only unsafe
consequence (undercount → premature delete) is caught by `INV_NO_LOSS`. The temporal `no-leak-forever` is
**bound-limited by the round cap** (an honest artifact — `GStartRound` is disabled at `MaxRound`, so a last-round
unreachable object is never collected; not a clean pass — see `RESULTS`).

**Two refinements the model surfaced — now folded into §5 as `W-REVALIDATE` and `W-TREE-BUILD`:**
- **F1 — the publish gate must re-validate dependencies' CURRENT physical state, not only the originally observed
  token.** An observed token can go stale between observation and publish — by delete-then-retire-entry-drop, or
  by another writer's same-content overwrite — so a gate keyed on the stale token misses the condemnation and a
  published ref can dangle. The model proves this load-bearing (the `sab_noretireview`/`sab_unconddelete`
  counterexamples) and captures it conservatively via a durable dead-token history; the **real protocol cannot
  keep such a registry** (retire entries drop by design), so the spec mechanism is **re-observation**
  (`W-REVALIDATE`, §5): stale-observed members are re-HEADed on retire-view refresh, keyed by the lemma that a
  delete can be in flight only for a token whose entry is still held. §7 R4 and the no-return argument step 4
  carry the matching amendments.
- **F2 — tree publish requires an explicit bottom-up build discipline** (`W-TREE-BUILD`, §5): a tree object only
  after its children exist; a tree ref only when the new tree's transitive closure is present and uncondemned.
  The model checks the one-level case; the transitive nested-subtree case remains a recorded residual.

**Inductive-invariant rung (Apalache) — complete.** The trimmed proof core
`CaIncarnationProofCore.tla` (single leader, `W-REVALIDATE` gate only, token-only dependencies) was
driven through Apalache 0.58.0 one-step induction to yield a 19-conjunct `IndInv` that is
**INDUCTIVE** at `|Writers|=2`, `|Shards|=1`, `|Hashes|=2`, `MaxToken=3`, `MaxRound=2`, `MaxLog=4`
(base PASS, step PASS — 45s/72s wall, reproduced; 12 CTI iterations). Full conjunct list, CTI
journal, and negative-control outcomes are in `CaIncarnationCore_RESULTS.md` (§ "Apalache induction
results"). Key findings: `InflightCurrentUnreferenced` is the irredundant heart conjunct — without
it the spared-branch orphan breaks the induction, and the original `InflightHeld`/`InflightVsRefs`
lemmas become corollaries of `InflightCurrentUnreferenced` + the fence-discipline family after
strengthening. The gate negative control (`WPublishNoReval` via `--next`) fails on `NoDangle` — a
machine-checked F1 witness: removing the `W-REVALIDATE` re-observation conjunct from `WPublish`
allows a stale dep on a deleted object to pass the gate and publish. The induction covers all states
at fixed constants (unbounded depth, bounded constants); constant-parametric generality is TLAPS
territory, for which `IndInv` and the CTI journal are the prepared input.

**B91 refresh (2026-06-12) — the model brought onto the amended protocol.** Two new flag-gated
stages: `EnableRegistry` (namespace registry + manifest creation: `W-REGISTER`, the R3 registry
fence ending the round's retiring, FENCE-TIME shard universe, absent-manifest minting,
registration-time gate floors) and `EnableEvStale` (evidence staleness: tokenless evidence carries
its recording view round; stale members must be re-observed; tree children validated through the
writer's OWN dependency set instead of a global presence oracle). Three new negative controls, all
counterexample-confirmed: `sab_noregistry` (skip `W-REGISTER` ⇒ dangle), `sab_foldtimeuniverse`
(fence the fold-time universe — the exact C++ hole fixed 2026-06-12 ⇒ dangle), `sab_noevreobserve`
(admit stale evidence ⇒ the Task-9 dangle). The refresh machine-derived THREE fixes: (1) the C++
fence-universe hole (`Gc::fence` iterated the fold-time registry; fixed to the committed
registry-fence universe); (2) the view-round COVERAGE PROPERTY — a view "at round R" must see ALL
round-R retire entries, which holds in the implementation (`gc/state.round` advances at R2
completion) and is now modeled (`ViewableRound`), and which forces the registry fence to END the
round's retiring; (3) blind `adoptTree` evidence was a dangle (whole-root adoption is now a COLD
REUSE — see `W-EVIDENCE` SCOPE above). All prior stages re-run green under the refreshed semantics
(state counts shrink — `ViewableRound` prunes unimplementable early views); all 12 sabotage
controls fire. Residuals: registry × split-brain and registry × evidence combined configs are not
run (state space); `CaIncarnationProofCore.tla` (Apalache) predates the amendments and is stale.

## 13. Open items and integration deltas {#open-items}

**Integration deltas vs the current branch (M1–M9 PoC):**
- `PoolCoordination` probe extended: enforced conditional delete (wrong token must fail), versioning-off check,
  token-type discovery; capability proof recorded in `_pool_meta`.
- Flat `refs/` objects → CAS shard manifests with embedded journal; `RefPayload` moves inside the manifest;
  `GcDelta`/`GcLayout` and the GC s1–s4 plan series are superseded by §7/§8.
- Pool format v2: the CHCA envelope replaces raw blob layout; `StoredObject`-level range plumbing (shared by
  the `header_len` shift, `pack_slice`, and inline reads); `FileCache` range caching used as-is.
- Keeper-free default for non-replicated tables; Keeper bindings as accelerators.

**Open items:**
- GCS binding implementation (header injection + generation extraction); fail-closed until probed.
- CI backend for delete-probe-passing GC tests: MinIO AIStor image, Ceph, or a generations-capable GCS fake
  (open-source MinIO cannot pass the v1 delete probe).
- Sub-shard count `N` defaults and the manifest size guard; thresholds for inline/pack/blob placement bands.
- Versioned mode (token = version/generation, delete-by-version) as a future explicit extension with its own
  modeling (delete markers, noncurrent metrics, lifecycle).
- FUSE phases (read-only first; pin-by-publishing-root; writing FUSE = §5).
- Metric wiring per §11; `system.*` tables answering "I dropped a table, why didn't S3 shrink?".

---

## Appendix A. Formal model sketch (TLA+-convertible) {#appendix}

### A.1 Constants {#a-constants}
```text
Hashes      finite set of content identities, e.g. {h1, h2, t1, t2}
Children    Hashes -> SUBSET Hashes            \* static tree structure
Writers     e.g. {w1, w2};  Leaders e.g. {L1, L2}
Shards      e.g. {s1, s2}
MaxToken    bound on incarnation tokens per key (e.g. 3)
MaxRound    bound on GC rounds
```

### A.2 Variables {#a-vars}
```text
\* ---- S3 durable ----
obj        \in [Hashes -> [present: BOOLEAN, token: Nat]]        \* current incarnation per key (one key per hash)
deletedTok \in [Hashes -> SUBSET Nat]                            \* history: tokens physically deleted (for INV_NO_RETURN)
manifest   \in [Shards -> [ver: Nat, fenceRound: Nat,
                           refs: SUBSET (Names \X Hashes),
                           journal: Seq([op: {Add, Rem}, name: Names, tree: Hashes, ver: Nat])]]
snap       \in [edges: SUBSET Edge, marker: SUBSET Hashes,       \* present-edge set + expansion markers
                cursor: [Shards -> Nat], gen: Nat]
retired    \subseteq [h: Hashes, tok: Nat, round: Nat]           \* unique-path append; dropped on outcome
outcomes   \subseteq [h: Hashes, tok: Nat, round: Nat, o: {Deleted, Absent, Replaced, Spared}]
gcState    \in [round: Nat, fenceVer: [Nat -> [Shards -> Nat]], ckptGen: Nat, lease: Leaders \cup {None}]
heartbeat  \in [Writers -> [seq: Nat, present: BOOLEAN]]
inflight   \subseteq [h: Hashes, tok: Nat]                       \* delete messages in flight, land ANY time later
\* ---- writer local ----
wDeps      \in [Writers -> SUBSET [h: Hashes, tok: Nat \cup {RootEvidence}]]
wRetireView\in [Writers -> Nat]                                  \* highest retire round refreshed
```

### A.3 Key actions (guard ⇒ effect) {#a-actions}
```text
W_Put(w, h)         body must verify logical_hash = h (W-SAME-CONTENT); fresh token (W-FRESH-TAG):
                    obj[h] := [present ↦ TRUE, token ↦ newTok(h)]; add (h, tok) to wDeps[w].
                    Modeled both as conditional (guarded absent/If-Match) AND unconditional overwrite.
W_Resurrect(w, h)   obj[h].present ∧ condemned(h, obj[h].token, wRetireView[w]) ⇒ same as W_Put (new token).
W_ResolveEvidence(w, h)  (h, RootEvidence) ∈ wDeps[w] ∧ retireViewHits(h, wRetireView[w]) ⇒ replace it with
                    (h, obj[h].token) if not condemned, else W_Resurrect — models W-EVIDENCE; the publish
                    guard rejects an unresolved RootEvidence member whose hash hits the retire view.
W_Publish(w, s, n)  GUARD: heartbeat[w].present ∧ wRetireView[w] ≥ manifest[s].fenceRound
                    ∧ ∀ d ∈ wDeps[w]: ¬condemnedInView(d, wRetireView[w])
                    EFFECT (CAS): manifest[s].ver++; refs += (n, T); journal ⊕ [Add, n, T, ver].
W_Drop(w, s, n)     CAS: refs −= (n, T); journal ⊕ [Rem, n, T, ver].
W_Crash(w)          heartbeat[w].present := FALSE (eventually); wDeps lost; uploads remain as debris.
GC_Fold(L)          fold journals (cursor, ver]; expand tree on first Add iff ∉ marker; snap.gen++;
                    cursors advance only forward (CAS).
GC_Retire(L, h)     inDegree(h) = 0 ∧ obj[h].present ⇒ retired ⊕ [h, obj[h].token, round].
GC_Fence(L)         ∀ s: manifest[s].fenceRound := round (CAS, ver++); record fenceVer[round][s].
GC_Delete(L, e)     GUARD: e ∈ retired ∧ foldedThroughFence(e.round) ∧ inDegree(e.h) = 0
                    EFFECT: inflight ⊕ (e.h, e.tok)   \* the delete is a MESSAGE; landing is a separate action
Land(d ∈ inflight)  obj[d.h].token = d.tok ⇒ obj[d.h].present := FALSE; deletedTok[d.h] ⊕ d.tok;
                    (tree ⇒ cascade strips child edges in round order — pipeline rule)
                    obj[d.h].token ≠ d.tok ⇒ no-op (412).
FullGC(L)           per-shard cut := manifest[s].ver at read; rebuild snap exactly through cuts;
                    checkpoint CAS fails if any cursor > cut; debris: ¬journalKnown(h) ∧ heartbeat of
                    buildOf(h) not advancing across GC-observed window ⇒ feed GC_Retire.
                    buildOf(h) = the core-header build_id (protocol-visible; never a diagnostic TLV).
```

### A.4 Invariants {#a-inv}
```text
INV_NO_DANGLE     ∀ (n, T) ∈ refs: obj[T].present ∧ ∀ c ∈ reachable(T): obj[c].present
INV_NO_RETURN     ∀ h, tok ∈ deletedTok[h]: [](obj[h].token ≠ tok)            \* deleted token never current again
INV_NO_LOSS       a Land that flips present := FALSE only ever hits (h, tok) with zero reachability at its
                  round's recheck (history-variable formulation)
INV_OVER_COUNT    snapInDegree(h) ≥ trueInDegree(h) at fold boundaries        \* may overcount, never undercount
INV_MONOTONE      cursors, fenceRound, snap.gen, ckptGen never decrease; fenceVer/cut immutable
NoLeakForever     (eventually-always unreachable ∧ journal-known) ~> eventually ¬present;
                  debris of dead builds ~> eventually ¬present
```

### A.5 Bounds and the headline scenarios {#a-bounds}
`Writers = {w1, w2}`, `Leaders = {L1, L2}`, `Shards = {s1}` (+ `{s1, s2}` for the cut/fence-ordering cases),
a 2-child tree, `MaxToken = 3`, `MaxRound = 3`. Headlines: the two fence horns; retired-old-token vs
newer-current-token; zombie `Land` after resurrect; cascade-vs-recreate pipeline order; full-GC cut-vs-cursor
CAS; wedged-heartbeat publish (heartbeat dead, commit path alive); drop/re-attach replay. Bounded model
checking — strong evidence within bounds, not a proof.
