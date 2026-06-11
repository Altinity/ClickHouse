---
description: 'Code-architecture design for rewriting the content-addressed PoC onto the incarnation-token spec: a self-contained DB::Cas core library (spec vocabulary, minimal token-aware backend seam, protocol-complete v1), a thin IMetadataStorage/IMetadataTransaction wiring layer, staged in-place cutover, and a three-tier test strategy with the TLA+ scenarios as in-memory protocol tests.'
sidebar_label: 'CA core refactor design'
sidebar_position: 3
slug: /superpowers/specs/ca-core-refactor-design
title: 'Content-Addressed Core Refactor — Cas Library + Wiring Design'
doc_type: 'guide'
---

# Content-Addressed Core Refactor — `Cas` Library + Wiring {#ca-core-refactor}

**Status:** approved design (brainstormed 2026-06-11). This is the CODE architecture for implementing the
protocol spec `2026-06-10-ca-incarnation-store-design.md` (the single source of truth for all protocol
behavior — this document never restates protocol rules, it places them in code). The current PoC
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, ~10.3k lines + 7.4k lines of gtests) was
never published; **no compatibility is preserved** — layout, formats, and tests are all replaceable.

## 1. Why a rewrite, not a cleanup {#why}

The PoC has two structural problems the protocol spec made terminal:

1. **The protocol is inside the ClickHouse adapters.** `ContentAddressedMetadataStorage` is a god object
   (path resolution + generation read machinery + relink pins + GC shared state exposed through accessors);
   `ContentAddressedTransaction` (2.3k lines) interleaves the protocol write path with all MergeTree path
   semantics. CAS behavior is untestable except through the disk harness.
2. **The protocol it implements is superseded.** The `Gc*` family is the pre-incarnation s1–s4 design
   (reverse index, epoch logs, generations + tombstones + resurrect-to-g+1, write-sessions-until-folded,
   in-process `gc_lock`); the read path carries `404→LIST` generation repair and `active` hints. The new
   spec deletes all of it (one key per hash, incarnation tokens, CAS shard manifests with embedded journal,
   fence/retire/re-observe, heartbeats).

So: reorganize and re-base in one move. Three decisions fixed during brainstorming:
**(D1) protocol-complete core v1** — write/read/publish/drop + shard manifests/journal + the full regular-GC
tail + heartbeats + probe + CHCA envelope; packs and the full-GC walk deferred (API slots reserved).
**(D2) minimal token-aware backend seam** (`Cas::Backend`, ~8 ops) — not `IObjectStorage` directly.
**(D3) in-place staged cutover** — core lands green beside the untouched PoC; ONE cutover milestone rewrites
the wiring and deletes the old code; SQL-level suites are the acceptance bar at that gate.

## 2. Layering {#layering}

```text
MergeTree / DataPartsExchange / DiskObjectStorageTransaction      (existing isContentAddressed switches)
        │ IMetadataStorage / IMetadataTransaction seam (the proven PoC seam — kept)
        ▼
WIRING  .../MetadataStorages/ContentAddressed/          namespace DB::ContentAddressed
        ContentAddressedMetadataStorage / ContentAddressedTransaction as THIN translators:
        ClickHouse path parsing, part semantics, placement/mutable-file policy, StoredObjects
        │ calls only the Cas:: public API
        ▼
CORE    .../MetadataStorages/ContentAddressed/Core/     namespace DB::Cas
        the protocol library — spec vocabulary, no ClickHouse paths, no IMetadataStorage types
        │ calls only Cas::Backend
        ▼
BACKEND Cas::ObjectStorageBackend (production, over IObjectStorage + per-backend token bindings)
        Cas::InMemoryBackend     (test: ENFORCES token semantics; fault injection)
```

Dependency direction is strict and enforced by review: the core never includes wiring headers; the wiring
never touches `Cas::Backend` or object keys directly. A dedicated `IDisk` implementation was considered and
rejected (vastly wider surface; the `IMetadataStorage` seam keeps the `DiskObjectStorage` read stack —
read buffers, `FileCache` — for free, re-proven through PoC M1–M9). Reorganize-in-place without a standalone
API was rejected (CAS tests would still need the disk harness; the tangle survives).

## 3. The core API {#core-api}

One file per concept, each independently reviewable. Sketch (signatures abbreviated; the spec rule each
enforces in comments):

```cpp
namespace DB::Cas {

struct Token { String value; TokenType type; };           // ETag / generation / (versioned mode later)

class Backend {                                           // ~8 ops; token semantics are THE contract
    GetResult      get(Key, Range = whole);               // bytes + observed Token
    HeadResult     head(Key);                             // exists? + Token
    PutOutcome     putIfAbsent(Key, Data);                // If-None-Match:*   (hygiene: create)
    PutOutcome     putOverwrite(Key, Data, Token);        // If-Match          (hygiene: resurrect)
    CasOutcome     casPut(Key, Data, optional<Token>);    // SAFETY-critical: root manifests only
    DeleteOutcome  deleteExact(Key, Token);               // SAFETY-critical: enforced 412 on mismatch
    ListPage       list(Prefix, cursor);                  // full-GC tier only; never on hot paths
};

class Store {                                             // one pool; open is fail-closed
    static StorePtr open(BackendPtr, PoolConfig);         // runs Cas::Probe + pool-format check
    BuildPtr       startBuild(BuildInfo);                 // W-HEARTBEAT durable before first PUT
    // read side — no GC awareness, no tokens (spec §6):
    Resolved       resolveRef(Namespace, RefName);        // → RefPayload{TreeId, sizes, mutable fields}
    Tree           readTree(TreeId);
    BlobLocation   locate(const TreeEntry &);             // (key, offset, size) for the read stack
    RefList        listRefs(Namespace);                   // merge of the namespace's N shard manifests
    void           dropRef(Namespace, RefName);           // ONE casPut: refs−− + journal '-' atomic
    void           updateRefPayload(Namespace, RefName, Mutator);
                   // refs-only CAS for the MUTABLE RefPayload fields of a committed part
                   // (txn_version/metadata_version rewrites — incl. a merge transaction touching
                   // covered parts); no reachability change ⇒ no journal record, shard_version++
    void           dropNamespace(Namespace);              // tombstone manifests, kept until folded
    // verbatim namespace files (format_version.txt …): putFile/getFile/listFiles — plain keys,
    // removed with dropNamespace; never content-addressed.
};

class Build {                                             // the writer protocol; §5 W-rules live HERE
    BlobRef  putBlob(BlobId, Source);                     // CHCA envelope + fresh tag (W-FRESH-TAG)
    BlobRef  reuseBlob(BlobId);                           // cold reuse: observe token + retire-view check
    BlobRef  adoptFromTree(TreeId, name);                 // carry-forward: live-root evidence (W-EVIDENCE)
    void     adoptTree(TreeId);                           // whole-tree adoption (FREEZE / detached / relink
                                                          // republish reuses the SAME TreeId; evidence on
                                                          // the tree root, closure via the gate)
    TreeId   putTree(entries);                            // bottom-up: children present+live (W-TREE-BUILD)
    void     publish(Namespace, RefName, TreeId, RefPayload);
             // ONE casPut (refs + journal atomic); gate: retire_view ≥ fence_round over the FULL
             // dependency set; on fence conflict → refresh view, RE-OBSERVE stale members
             // (W-REVALIDATE: keep / adopt / re-create), resurrect condemned ones, retry.
    void     abandon();                                   // uploads become debris (heartbeat-gated)
};

class Gc {                                                // leader-paced; SAFE under split-brain
    RoundReport runRegularRound();                        // fold → retire(HEAD token) → fence(ALL shards)
                                                          // → recheck(fold-through-fence) → deleteExact
                                                          // → outcomes {deleted|absent|replaced|spared}
    // full-GC walk + debris reclaim: deferred milestone; API slot reserved.
};
}   // formats: Cas::Envelope (CHCA core header + provenance TLV), Cas::TreeCodec, Cas::RootShardCodec;
    // keys: Cas::Layout (key construction only); ids: Cas::BlobId / Cas::TreeId (strong-typed strings,
    // the Identifiers.h pattern ported).
```

Notes. **Namespaces and ref names are opaque to the core**; the wiring maps `(table_uuid, part_name)` →
`(RootNamespace, ref_name)`; sharding within a namespace is core (`hash(ref_name) % N`). **The read path
returns sizes with every reference** — the wiring builds `StoredObjects` without a single HEAD. The gate,
re-observation, and the in-flight-disjunction reasoning are `Build`-internal: a wiring author cannot
mis-implement them. `Cas::TreeEntry` has three placements (`inline`, `blob`, `pack_slice`) — `pack_slice`
is encoded and validated from day one, produced by nobody until the packs milestone.

## 4. The wiring layer {#wiring}

`ContentAddressedMetadataStorage`: holds a `Cas::StorePtr`; implements the `IMetadataStorage` read surface
by parsing ClickHouse paths and calling `resolveRef`/`readTree`/`locate`/`listRefs`; `startup`/`shutdown`
open the store and drive a GC scheduler thread calling `Gc::runRegularRound`. No protocol state, no
internals-exposing accessors. `DataPartsExchange` gets a small purpose-built façade (pool identity +
adopt-by-id entry point) instead of `dynamic_cast` into the concrete class.

`ContentAddressedTransaction`: keeps the `IMetadataTransaction` shape; accumulates ClickHouse operations and
maps them to **one `Cas::Build` per written part** at commit. The PoC's path-semantics inventory survives as
an explicit dispatch (each case encodes a real MergeTree behavior — this part stays closest to the existing
code):

| ClickHouse operation | Wiring translation |
|---|---|
| `writeFile` (content file) | `Build::putBlob` (placement policy may choose `inline` into the tree) |
| `writeFile`/`replaceFile` (mutable per-part file) | staged into `RefPayload` fields — **the sidecar `.meta` object family is deleted**; mutable state lives in the shard manifest per the spec |
| `createHardLink` (carry-forward) | `Build::adoptFromTree` |
| `moveDirectory` tmp→final | re-pin the publish target (ref name) |
| `moveDirectory` → detached / ATTACH from detached | `publish` into / out of the `detached` namespace + `dropRef` — ref-level ops, never object surgery |
| `moveDirectory` staged projection rename | rekey staged entries in the `Build` |
| `moveDirectory` table rename | republish refs under the new namespace + drop old (journaled) |
| committed-part rename (`delete_tmp_…`) | `dropRef` + `publish` same tree |
| `removeRecursive` / `removeDirectory` (part / table) | `dropRef` / `dropNamespace` |
| FREEZE | `publish` into a `shadow/<backup>` namespace |
| read-your-writes (`tryGetInFlight*`) | served from the `Build`'s staged state |
| fetch/relink (replication) | `startBuild` + adopt-by-id + `publish` — the 4-step pin protocol and `WriteSession` vanish; the publish gate provides the safety |

Wiring-owned **policy tables**: inline-vs-blob size thresholds; the mutable-file name set; the verbatim
table-file name set. Policy is data, not protocol.

## 5. Deletion inventory {#deletions}

Removed at the cutover milestone, not ported: `ContentAddressedGC`, `ContentAddressedGCThread` (replaced by
the thin scheduler), `GcLogWriter`, `GcDelta`, `GcLayout`, `GcCompaction`, `WriteSession`,
`PoolCoordination` (reborn as `Cas::Probe`), the old `RefPayload` codec, generation machinery
(`resolveBlobGenKey*`, `repair*On404`, `active` hints, gen caches, `resolveAndResurrectGeneration`),
in-process `gc_lock` / `in_flight_pinned_blobs` / `InMemoryBlobRefIndex`, the `.meta` sidecar key family,
and `PoolPaths`' parsing half (key construction moves to `Cas::Layout`; ClickHouse path parsing stays in
wiring as a focused `PartPathParser`). Old gtests pinning that layout (`gtest_content_addressed_gc_s2/s3/s4`
and the layout-coupled bulk of `gtest_content_addressed_metadata`) are deleted with the code; their semantic
intent returns as core/wiring tests below.

## 6. Error handling {#errors}

Fail-closed everywhere, inherited from the spec: `Store::open` refuses the pool on any probe failure
(including the enforced wrong-token-delete check) or format mismatch; a live ref resolving to a missing key
is a storage exception (`INV-NO-DANGLE` surfaced, never substituted); an unknown critical envelope extension
fails the read; `Build::publish` failures leave debris, never partial roots. The wiring adds no fallback
paths: a path it cannot classify is an exception, not a guess.

## 7. Testing {#testing}

Three tiers, each at its own seam:

1. **Core protocol tests** (in-memory, milliseconds — the new centerpiece): the TLA+ scenario list as
   gtests against `Cas::InMemoryBackend` with fault injection (held/delayed `deleteExact` landing later,
   CAS conflicts, crash-shaped abandons): both fence horns; retired-old-token vs newer-current; zombie
   delete after resurrect; spared-entry orphan + the in-flight disjunction; W-REVALIDATE re-observation on
   fence conflict; wedged-heartbeat publish self-heal; drop/re-attach replay; cascade ordering. Negative
   controls in the TLC spirit: a fake configured NOT to enforce token semantics must be rejected by the
   probe; protocol-rule bypasses must be inexpressible through the API (else runtime-asserted and tested).
2. **Backend contract suite**: one parameterized battery (token semantics, enforced 412, CAS atomicity,
   list-after-write) run against `InMemoryBackend` AND `ObjectStorageBackend`-over-`LocalObjectStorage`,
   proving the fake matches production semantics.
3. **Wiring tests**: the dispatch table of §4 exercised through the `IMetadataStorage`/`IMetadataTransaction`
   seam against the in-memory backend (replacing, case by case, the salvageable intent of the old metadata
   gtest). **Acceptance**: the existing SQL-level suites (the CA-default stateless job, integration tests)
   green at the cutover gate, unchanged.

**Empirical backend finding (2026-06-11, recorded in the protocol spec's backend table):** MinIO OSS — both
the CI image (`RELEASE.2024-09-13`) and the final pre-archive release (`RELEASE.2025-09-07`) — **silently
ignores `If-Match` on DELETE and deletes anyway** (probed live: wrong token → 204 → object gone). This is
exactly the failure mode the enforced probe catches. **Counter-finding (same day, same method): RustFS
1.0.0-beta.8 passes the complete safety-critical battery** — DELETE `If-Match` enforced (wrong token → 412,
object survives), `If-None-Match:*` create enforced, `If-Match` CAS PUT enforced — making it the leading
open-source CI candidate for the GC-e2e lane (caveats: beta; strictly requires quoted ETags in conditional
headers, rustfs#1458 — the binding sends tokens exactly as observed). AIStor and Ceph remain alternates.
Everything below e2e runs on the in-memory/Local backends, which enforce token semantics natively.

## 8. Milestones {#milestones}

Each lands green before the next starts (D3); each gets its own implementation plan via writing-plans:

- **M-C1 — backend + formats**: `Cas::Backend` + `InMemoryBackend` + `ObjectStorageBackend`,
  `Envelope`/`TreeCodec`/`RootShardCodec`, `Layout`, `Probe`. Green: backend contract suite + codec tests.
- **M-C2 — store + build (write/read)**: `Store` open/resolve/list/drop, `Build` with the full publish gate,
  heartbeats, namespaces incl. detached/shadow, verbatim files. Green: writer/reader protocol tests.
- **M-C3 — GC regular tail**: `Gc::runRegularRound` (fold/retire/fence/recheck/deleteExact + outcomes),
  retire-set plumbing into the `Build` gate. Green: the full model-scenario battery incl. fault injection.
- **M-W — the cutover**: wiring rewrite onto the core, relink retarget, `DataPartsExchange` façade,
  deletions of §5. Green: wiring tests + the SQL-level acceptance suites.
- **M-F — follow-ups (separately planned)**: full-GC walk + debris reclaim, packs (+ repack), AIStor/Ceph CI
  lane for GC e2e, observability surfacing (`system.*` views over `RoundReport`/retire backlog).

## 9. Open items {#open-items}

- CI GC-e2e lane backend: RustFS (empirically verified, open-source, leading candidate) vs AIStor
  (licensing question) vs Ceph (heavyweight) — infra, M-F.
- Sub-shard count `N` default per namespace and the manifest size guard (spec §4) — constants chosen in M-C2
  with a recorded rationale.
- GCS token binding implementation (header injection) — deferred with the probe failing closed, per the spec.
- Whether `DataPartsExchange`'s relink cookie/protocol fields change shape when retargeted — decided in M-W
  (wire format is replica-internal on this branch; no compatibility constraint).
