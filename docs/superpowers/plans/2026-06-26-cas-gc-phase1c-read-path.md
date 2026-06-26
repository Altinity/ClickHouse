---
description: "Query-hot read path over root-local part manifests for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 1c (read path)"
sidebar_position: 5
slug: /superpowers/plans/2026-06-26-cas-gc-phase1c-read-path
title: "Phase 1c — Read Path Over Part Manifests — Implementation Plan"
doc_type: reference
---

# Phase 1c — Read Path Over Part Manifests — Implementation Plan {#phase-1c-read-path-over-part-manifests-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Each step is bite-sized (one 2–5 min action), uses checkbox (`- [ ]`) syntax, and every task ends with a commit. Read `2026-06-26-cas-gc-redesign-overview.md` first, then this. **Gate:** Phase 0 model GREEN. **Depends on:** Phase 1a (`CasManifestId`, `CasManifestCodec`, `CasLayout::manifestKey`); coordinates with Phase 1b's `RootRef`.

**Goal:** Make the query-hot read path resolve a ref to a `ManifestId`, read the single immutable part manifest object, enforce `refMatchesBody` / `manifestNamespaceMatches`, and serve path lookup / directory listing from the manifest entries — replacing the content-addressed `trees/<hash>` read path. Rewire `ContentAddressedMetadataStorage`'s read helpers onto the new surface.

**Architecture:** `resolveRef` returns a `ManifestId` (the `RootRef.manifest_ref` interpreted inside the owning `RootNamespace`) plus the mutable per-ref payload, unchanged otherwise. `readManifest` replaces `readTree`: it derives the object key via `CasLayout::manifestKey`, reads and decodes a `PartManifest`, fails closed on a committed ref naming a missing body (`FILE_DOESNT_EXIST`) and on a body that fails `refMatchesBody` / `manifestNamespaceMatches` (`CORRUPTED_DATA`). Path lookup (`lookupPath`) and directory listing (`listDirectory`) read canonical-path-ordered `ManifestEntry` records (Resolved OQ3: no `DirectoryIndex` yet). The decode cache is keyed by `(ManifestId, Token)`, not by content hash — each publish has a unique id, so cross-id sharing is gone. This is the intentional read-path tradeoff from spec §Read Path Scope.

**Tech stack:** C++ (ClickHouse coding standards, Allman braces); gtest unit oracles against the `Cas::Backend` seam; `build/src/unit_tests_dbms` with `--gtest_filter='CasStore.*'` / `'CaWiring*'`.

**Source spec:** `docs/superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md` (rev. 13), sections §Read Path Scope, §Object Identity And Ownership (the `refMatchesBody` / `manifestNamespaceMatches` checks), §Core Principle.

## Global Constraints {#global-constraints}

*Every task below implicitly includes this section (copied verbatim from the overview).*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in a phase may begin until that phase's TLA+ gate is green.
- **R1 — bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 — target-shardable.** Default `gc_shards = 1`; sharded mode is optional (Phase 4).
- **R3 — simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, trimmed.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden (per `feedback_ca_no_compat_scaffolding_predev`).

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source — **never** `GET`s the condemned object (per `feedback_ca_resurrect_invariant`);
- GC must never throw/fail-closed on a 404 during fold (record what you can and continue — per `feedback_ca_gc_never_throw_on_404`).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into a `build_*` directory (here: `build`). Always redirect ninja output to `<build_dir>/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>[.sh]`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'` (exact target/filter confirmed below from the C++ ground-truth report).

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- **OQ1 (manifest fields).** The body carries `header`, `ref` (`ManifestRef`), `root_namespace_id`, `payload_digest` (integrity/debug only — never a key, never dedup, never in-degree), and `entries`. The read path validates `ref`/`root_namespace_id` against the lookup context and ignores `payload_digest` for routing.
- **OQ3 (internal indexing).** Phase 1 stores `entries` in canonical path order. The optional `DirectoryIndex` is **deferred and off by default**; `lookupPath` / `listDirectory` operate directly over the canonical-path-ordered entries. Add a `DirectoryIndex` only when `listDirectory` profiles demand it.
- **Read-path tradeoff (spec §Read Path Scope).** The old per-process tree decode cache shared by content hash is gone. Each publish uses a unique `ManifestId`, so the manifest cache keyed by `(ManifestId, Token)` has **less sharing across publishes**. This is intentional: blob payloads still dedup; manifest metadata is small and immutable.

## Canonical Contract {#canonical-contract}

*Consume Phase 1a + 1b verbatim. EMIT Phase 1c's read-path changes. Only these type names are used in this plan.*

**Consumed from Phase 1a** (`CasManifestId.h`, `CasManifestCodec.h`, `CasLayout`):
- `ManifestRef` — compact root-journal reference (`writer_instance_id`, `build_sequence`, `manifest_instance_id`).
- `ManifestId { root_namespace; ref; }` — namespace-qualified identity (the `ManifestId` field is `root_namespace`, exactly as Phase 1a emits it; do not rename it to `root_namespace_id` — that `_id` suffix belongs to the `PartManifest` *body* field). Ordering + hash provided by Phase 1a.
- `ManifestEntry { path; placement; blob_hash; blob_size; inline_bytes; }`.
- `PartManifest { ref; root_namespace_id; payload_digest; entries; }`.
- `decodePartManifest(...)` — decode a manifest body to a `PartManifest`.
- `refMatchesBody(journal_ref, body)` — the journal `ManifestRef` equals the body `ref`.
- `manifestNamespaceMatches(owning_ns, body)` — the body `root_namespace_id` equals the owning namespace.
- `CasLayout::manifestKey(ManifestId)` — derive the object key.

**Consumed from Phase 1b** (`CasRootShardCodec`):
- `RootRef { ref_name; manifest_ref; mutable_files; published_at_ms; }` — the committed ref record `resolveRef` reads (the current `RefPayload`'s replacement).

**EMITTED by Phase 1c** (in `CasStore.h` / `CasStore.cpp`):
- `struct Resolved { ManifestId manifest_id; uint64_t manifest_size; std::map<String, String> mutable_files; uint64_t published_at_ms; };` (rename `tree_id`→`manifest_id`; the `RootRef.manifest_ref` + the owning namespace form the `ManifestId`).
- `std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false)` — **unchanged signature**, new return field.
- `PartManifest readManifest(const ManifestId & id)` — replaces `readTree`. Derive key via `CasLayout::manifestKey`, `get`, `decodePartManifest`; enforce `refMatchesBody(id.ref, body)` and `manifestNamespaceMatches(id.root_namespace, body)` (mismatch ⇒ `CORRUPTED_DATA`); fail closed (`FILE_DOESNT_EXIST`) when a committed ref names a missing body.
- `std::optional<ManifestEntry> lookupPath(const PartManifest &, const String & path)`.
- `std::vector<ManifestEntry> listDirectory(const PartManifest &, const String & dir_prefix)` over canonical-path-ordered entries (Resolved OQ3: no `DirectoryIndex` yet).
- Decode cache keyed by `(ManifestId, Token)` (`manifest_cache`); **no cross-id sharing** (each publish has a unique id ⇒ less sharing, intentional).

## C++ Ground Truth (current code) {#c-ground-truth-current-code}

Confirmed against the tree at this branch:

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
  - `struct Resolved { TreeId tree_id; uint64_t tree_size = 0; std::map<String, String> mutable_files; uint64_t published_at_ms = 0; };` (`:58-64`).
  - `std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false);` (`:118`).
  - `std::vector<TreeEntry> readTree(const TreeId & id);` (`:119`); `BlobLocation locate(const TreeEntry & entry) const;` (`:120`); `std::map<String, Resolved> listRefs(const RootNamespace & ns);` (`:121`).
  - Tree decode cache: `tree_cache_mutex`, `std::unordered_map<String, std::shared_ptr<const std::vector<TreeEntry>>> tree_cache;` keyed by tree-id hex, `TREE_CACHE_MAX_ENTRIES = 16384`, wholesale clear on overflow (`:317-319`).
  - `#include <...Core/CasTreeCodec.h>` (`:9`).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp`
  - `resolveRef` (`:403`) routes via `readShardDecoded`, reads `RefPayload`, builds `Resolved` (`:428-433`); emits a `RefResolve` event with `object_hash = u128ToHex(payload.tree_id)` (`:416-427`).
  - `readTree` (`:436`): tree cache lookup (`:440-445`); missing body ⇒ `FILE_DOESNT_EXIST` (`:450-468`); `decodeEnvelopeHeader(..., ObjectKind::Tree)`, key↔hash + `domain_id` checks ⇒ `CORRUPTED_DATA` (`:472-514`); `decodeTree` + cache populate (`:516-526`).
  - `locate(TreeEntry)` (`:529`) → `BlobLocation` for `Placement::Blob`, throws for `Inline`/`Subtree`.
  - `listRefs` (`:551`) merges all shards, builds `Resolved` per ref (`:562-567`).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
  - `resolveRouted(const Route & r)` (`:488`) → `std::make_pair(*resolved, store()->readTree(resolved->tree_id))` (`:495`). Return type today is `std::optional<std::pair<Cas::Resolved, std::vector<Cas::TreeEntry>>>`.
  - Read helpers iterate `rt->second` (a `std::vector<Cas::TreeEntry>`) by `entry.name` (`:536-538`, and the `getStorageObjects`/`getFileSize` paths near `:866`, `:952`).
  - `liveNamespace(uuid)` (`:439`); `listRefs` / `locate` callsites at `:501`, `:866`, `:952`, etc.
- Tests: `src/Disks/tests/gtest_cas_store.cpp` (suite `CasStore`, e.g. `ResolveReadLocateRoundTrip` `:314`, `ReadTreeFailsClosed` `:456`, `ListRefsMergesAllShards` `:421`); `src/Disks/tests/gtest_ca_wiring.cpp` (suites `CaWiringRead`/`CaWiringWrite`/`CaWiringRoute`, e.g. `ResolvesPublishedPart` `:210`); helpers in `src/Disks/tests/cas_test_helpers.h` (`writeBlobRaw`, `publishRaw`, `shardOfForTest`, `expectThrowsCode`, `CountingBackend`).
- Build: `build/src/unit_tests_dbms` exists. Test binary + filter: `build/src/unit_tests_dbms --gtest_filter='CasStore.*'` (and `'CaWiring*'`).

> **Phase 1a/1b dependency note.** This plan writes against the canonical contract type names. If Phase 1a/1b landed equivalent helpers under slightly different spellings (e.g. a free `decodePartManifest` vs a `CasManifestCodec::decode`), the implementer adapts the call sites to the names actually present — the **behavior** (key via `manifestKey`, decode, `refMatchesBody` + `manifestNamespaceMatches`, fail-closed missing committed body, `(ManifestId, Token)` cache) is what this plan specifies. Do not reintroduce `TreeId`/`readTree`/`CasTreeCodec` on the read path.

## File Structure {#file-structure}

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` — `Resolved` field rename; replace `readTree` decl with `readManifest`; add `lookupPath` / `listDirectory` decls; replace the tree decode cache members with the `(ManifestId, Token)` manifest cache; swap the `CasTreeCodec.h` include for the Phase 1a manifest headers.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` — `resolveRef` returns `ManifestId`; `readManifest` body; `lookupPath` / `listDirectory` bodies; cache plumbing; keep `locate` working off `ManifestEntry`.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (+ `.h` decl of `resolveRouted`) — `resolveRouted` returns `std::pair<Cas::Resolved, Cas::PartManifest>`; read helpers use `lookupPath` / `listDirectory` instead of iterating a `std::vector<TreeEntry>`; `locate` is fed a `ManifestEntry`.
- Modify: `src/Disks/tests/gtest_cas_store.cpp` — add the Phase 1c `CasStore` tests (Tasks 1–4).
- Modify: `src/Disks/tests/cas_test_helpers.h` — add a `writeManifestRaw` fixture and a `publishRaw` overload taking a `RootRef` (mirrors the existing `writeTreeRaw` / `publishRaw`) once Phase 1a/1b codecs are present.

## Tasks {#tasks}

### Task 1: `Resolved` field rename + `resolveRef` returns `ManifestId` {#task-1-resolved-field-rename-resolveref-returns-manifestid}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp`
- Modify: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces produced:** `struct Resolved { ManifestId manifest_id; ... }`; `resolveRef` returning the new field.

- [ ] **Step 1: Swap the header include and rewrite `struct Resolved`.** In `CasStore.h`, replace the `CasTreeCodec.h` include with the Phase 1a manifest headers, and replace the `Resolved` struct.

Replace this include (`:9`):
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
```
with:
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
```

Replace the `Resolved` struct (`:58-64`) with:
```cpp
struct Resolved
{
    /// The namespace-qualified identity of the part manifest this ref names. The owning RootNamespace
    /// + the RootRef.manifest_ref form the ManifestId (the ref carries no namespace itself — that comes
    /// from the owning root context, spec §Object Identity And Ownership).
    ManifestId manifest_id;
    uint64_t manifest_size = 0;
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
};
```

- [ ] **Step 2: Rebuild `resolveRef`'s return in `CasStore.cpp`.** Replace the `Resolved{...}` construction (`:428-433`) so it forms a `ManifestId` from the owning namespace and the ref's `manifest_ref`. Also update the `RefResolve` event's `object_hash` to the manifest instance id (the body hash is no longer the identity).

Replace (`:428-433`):
```cpp
    return Resolved{
        .tree_id = TreeId(u128ToHex(payload.tree_id)),
        .tree_size = payload.tree_size,
        .mutable_files = payload.mutable_files,
        .published_at_ms = payload.published_at_ms,
    };
```
with:
```cpp
    return Resolved{
        .manifest_id = ManifestId{.root_namespace = ns, .ref = payload.manifest_ref},
        .manifest_size = payload.manifest_size,
        .mutable_files = payload.mutable_files,
        .published_at_ms = payload.published_at_ms,
    };
```
and update the event (`:422-423`) to address the manifest by its instance id rather than a content hash:
```cpp
        _ev0.object_kind = CasEventObjectKind::Tree;
        _ev0.object_hash = manifestInstanceHex(payload.manifest_ref);
```
(`manifestInstanceHex` is the Phase 1a helper that hex-renders `manifest_instance_id`; if Phase 1a exposes it differently, render the instance id with the helper that is present. Leave `CasEventObjectKind::Tree` — the event-kind enum is out of scope for this phase.)

- [ ] **Step 3: Update `listRefs` to the new field.** Replace the `Resolved{...}` in `listRefs` (`:562-567`) with the `ManifestId` form (same shape as Step 2, but the namespace is the loop's `ns`):
```cpp
            result.emplace(ref_name, Resolved{
                .manifest_id = ManifestId{.root_namespace = ns, .ref = payload.manifest_ref},
                .manifest_size = payload.manifest_size,
                .mutable_files = payload.mutable_files,
                .published_at_ms = payload.published_at_ms,
            });
```

- [ ] **Step 4: Write the gtest (failing first).** Add to `src/Disks/tests/gtest_cas_store.cpp`. This asserts `resolveRef` returns the `ManifestId` built from the owning namespace + the ref's `manifest_ref` (uses the Phase 1b `RootRef` and a `publishRaw` overload that places a `RootRef`).
```cpp
TEST(CasStore, ResolveReturnsManifestId)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};

    const ManifestRef ref{.writer_instance_id = "srv-a", .build_sequence = 1042, .manifest_instance_id = u128Of("inst-1")};
    RootRef rr{.ref_name = "part_1", .manifest_ref = ref, .mutable_files = {{"txn_version.txt", "42"}}, .published_at_ms = 1700000000};
    publishRaw(*b, layout, ns, shardOfForTest("part_1", s->poolMeta().root_shards), rr);

    auto r = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id.root_namespace, ns);
    EXPECT_EQ(r->manifest_id.ref.manifest_instance_id, u128Of("inst-1"));
    EXPECT_EQ(r->manifest_id.ref.build_sequence, 1042u);
    EXPECT_EQ(r->mutable_files.at("txn_version.txt"), "42");
}
```

- [ ] **Step 5: Add the `RootRef` `publishRaw` overload + run.** In `cas_test_helpers.h`, add a `publishRaw` overload (and a `writeManifestRaw` fixture used by Task 2) that builds a `RootShard` carrying a `RootRef`, mirroring the existing `RefPayload` overload. Then build and run.

Run:
```bash
ninja -C build unit_tests_dbms > build/build.log 2>&1 ; echo "ninja exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasStore.ResolveReturnsManifestId' > build/test_resolve_manifestid.log 2>&1 ; echo "gtest exit=$?"
```
Expected (analyze `build/build.log` and `build/test_resolve_manifestid.log` with a subagent): build succeeds; the log ends with
```
[  PASSED  ] 1 test.
```

- [ ] **Step 6: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "CA GC phase1c: Resolved carries ManifestId; resolveRef forms it from owning ns + RootRef.manifest_ref"
```

---

### Task 2: `readManifest` via `manifestKey` + body validation + fail-closed missing committed body {#task-2-readmanifest-via-manifestkey-body-validation-fail-closed-missing-committed-body}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp`
- Modify: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces produced:** `PartManifest readManifest(const ManifestId & id)`.

- [ ] **Step 1: Replace the `readTree` declaration.** In `CasStore.h` replace (`:119`):
```cpp
    std::vector<TreeEntry> readTree(const TreeId & id);           /// validates envelope, kind, key↔hash
```
with:
```cpp
    /// Read the single immutable part manifest named by `id`. Derives the key via CasLayout::manifestKey,
    /// decodes the body, and fails CLOSED: a committed ref naming a missing body throws FILE_DOESNT_EXIST
    /// (INV-NO-DANGLE surfaced on the read path); a body whose `ref` ≠ id.ref (refMatchesBody) or whose
    /// `root_namespace_id` ≠ id.root_namespace (manifestNamespaceMatches) throws CORRUPTED_DATA — the
    /// ref is addressing the wrong object, or a cross-namespace dangle. Caching is Task 4.
    PartManifest readManifest(const ManifestId & id);
```

- [ ] **Step 2: Write the `readManifest` body in `CasStore.cpp`.** Replace the whole `readTree` function (`:436-527`) with `readManifest`. (The `(ManifestId, Token)` cache is added in Task 4 — this task reads + validates fresh each call so the validation is testable in isolation.)
```cpp
PartManifest Store::readManifest(const ManifestId & id)
{
    /// A live ref naming a missing manifest body is INV-NO-DANGLE (spec §Read Path Scope: "fail-closed
    /// behavior when a committed ref names a missing manifest"). Never substitute an empty manifest.
    const String key = pool_layout.manifestKey(id);
    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
    {
        if (hasEventSink())
        {
            CasEvent _ev1;
            _ev1.type = CasEventType::ReadMissing;
            _ev1.object_kind = CasEventObjectKind::Tree;
            _ev1.object_hash = manifestInstanceHex(id.ref);
            _ev1.outcome = "missing";
            _ev1.reason = "live ref names manifest but its object is missing (INV-NO-DANGLE)";
            _ev1.detail = {{"code", "FILE_DOESNT_EXIST"}, {"site", "readManifest"}};
            emitEvent(_ev1);
        }
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "live ref names manifest at {} but its object is missing — INV-NO-DANGLE", key);
    }

    PartManifest body = decodePartManifest(object->bytes);

    /// refMatchesBody: the journal ManifestRef must equal the body's self-described `ref`. A mismatch
    /// means the ref addresses the WRONG object (spec §Object Identity And Ownership).
    if (!refMatchesBody(id.ref, body))
    {
        if (hasEventSink())
        {
            CasEvent _ev2;
            _ev2.type = CasEventType::CorruptDecode;
            _ev2.object_kind = CasEventObjectKind::Tree;
            _ev2.object_hash = manifestInstanceHex(id.ref);
            _ev2.outcome = "corrupt";
            _ev2.reason = "manifest body `ref` does not match the journal ManifestRef (refMatchesBody)";
            _ev2.detail = {{"code", "CORRUPTED_DATA"}, {"site", "readManifest"}};
            emitEvent(_ev2);
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS manifest at {} body ref does not match the journal ManifestRef — refMatchesBody", key);
    }

    /// manifestNamespaceMatches: the body's root_namespace_id must equal the owning root namespace. A
    /// mismatch is a cross-namespace dangle and would hand the debris sweep the wrong authority.
    if (!manifestNamespaceMatches(id.root_namespace, body))
    {
        if (hasEventSink())
        {
            CasEvent _ev3;
            _ev3.type = CasEventType::CorruptDecode;
            _ev3.object_kind = CasEventObjectKind::Tree;
            _ev3.object_hash = manifestInstanceHex(id.ref);
            _ev3.outcome = "corrupt";
            _ev3.reason = "manifest body root_namespace_id does not match the owning namespace (manifestNamespaceMatches)";
            _ev3.detail = {{"code", "CORRUPTED_DATA"}, {"site", "readManifest"}};
            emitEvent(_ev3);
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS manifest at {} body root_namespace_id does not match the owning namespace — manifestNamespaceMatches", key);
    }

    return body;
}
```

- [ ] **Step 3: Add `writeManifestRaw` fixtures producing both valid and mismatched bodies.** In `cas_test_helpers.h` add a `writeManifestRaw(backend, layout, ManifestId id, std::vector<ManifestEntry> entries, ...)` that encodes a `PartManifest` whose body `ref`/`root_namespace_id` equal the id (the valid round trip), plus two variants that deliberately write a body whose `ref` differs (for `refMatchesBody`) and whose `root_namespace_id` differs (for `manifestNamespaceMatches`). Mirror `writeTreeRaw`'s "write through the same codec the Store reads" discipline.

- [ ] **Step 4: Write the gtest (four cases).** Add to `gtest_cas_store.cpp`. The four cases match the deliverable: read ok; ref-mismatch ⇒ throws; ns-mismatch ⇒ throws; missing committed body ⇒ throws. (A missing *precommit* body is a write/GC concern — not exercised here; this read path only sees committed refs and must fail closed for them.)
```cpp
TEST(CasStore, ReadManifestValidatesBodyAndFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};

    const ManifestRef ref{.writer_instance_id = "srv-a", .build_sequence = 7, .manifest_instance_id = u128Of("inst-ok")};
    const ManifestId id{.root_namespace = ns, .ref = ref};

    /// 1) read ok: body ref + root_namespace_id agree with the id.
    std::vector<ManifestEntry> entries;
    entries.push_back(ManifestEntry{.path = "data.bin", .placement = Placement::Blob,
        .blob_hash = u128Of("hello world"), .blob_size = 11, .inline_bytes = ""});
    writeManifestRaw(*b, layout, id, entries);
    auto m = s->readManifest(id);
    EXPECT_EQ(m.entries.size(), 1u);

    /// 2) ref-mismatch ⇒ CORRUPTED_DATA (body carries a different ManifestRef than the id).
    const ManifestRef ref_bad{.writer_instance_id = "srv-a", .build_sequence = 7, .manifest_instance_id = u128Of("inst-ref-bad")};
    const ManifestId id_ref{.root_namespace = ns, .ref = ref_bad};
    writeManifestRawWithBodyRef(*b, layout, id_ref, /*body_ref=*/ref, entries);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(id_ref); });

    /// 3) ns-mismatch ⇒ CORRUPTED_DATA (body root_namespace_id is a different namespace than the owner).
    const ManifestRef ref_ns{.writer_instance_id = "srv-a", .build_sequence = 7, .manifest_instance_id = u128Of("inst-ns-bad")};
    const ManifestId id_ns{.root_namespace = ns, .ref = ref_ns};
    writeManifestRawWithBodyNs(*b, layout, id_ns, /*body_ns=*/RootNamespace{"srv1/other"}, entries);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(id_ns); });

    /// 4) missing committed body ⇒ FILE_DOESNT_EXIST (no object at the key).
    const ManifestRef ref_gone{.writer_instance_id = "srv-a", .build_sequence = 7, .manifest_instance_id = u128Of("inst-gone")};
    const ManifestId id_gone{.root_namespace = ns, .ref = ref_gone};
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readManifest(id_gone); });
}
```

- [ ] **Step 5: Build + run.**
```bash
ninja -C build unit_tests_dbms > build/build.log 2>&1 ; echo "ninja exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasStore.ReadManifestValidatesBodyAndFailsClosed' > build/test_readmanifest.log 2>&1 ; echo "gtest exit=$?"
```
Expected (analyze logs with a subagent): build succeeds; the test log ends with
```
[  PASSED  ] 1 test.
```

- [ ] **Step 6: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "CA GC phase1c: readManifest via manifestKey + refMatchesBody/manifestNamespaceMatches, fail-closed missing committed body"
```

---

### Task 3: `lookupPath` / `listDirectory` over canonical-path-ordered entries {#task-3-lookuppath-listdirectory-over-canonical-path-ordered-entries}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp`
- Modify: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces produced:** `std::optional<ManifestEntry> lookupPath(...)`; `std::vector<ManifestEntry> listDirectory(...)`.

- [ ] **Step 1: Declare the two helpers.** In `CasStore.h`, right after the `readManifest` declaration, add:
```cpp
    /// Path lookup over a decoded part manifest's canonical-path-ordered entries (Resolved OQ3: no
    /// DirectoryIndex yet). Returns the entry whose `path` equals `path`, or nullopt.
    std::optional<ManifestEntry> lookupPath(const PartManifest & manifest, const String & path) const;
    /// Directory listing: every entry whose `path` lies under `dir_prefix`, in canonical path order.
    /// `dir_prefix` is matched as a path prefix; the caller collapses to first path segments (the
    /// wiring's listDirectory does the segment collapse, as it did for tree entries).
    std::vector<ManifestEntry> listDirectory(const PartManifest & manifest, const String & dir_prefix) const;
```

- [ ] **Step 2: Implement both in `CasStore.cpp`** (place them just after `readManifest`). Entries are canonical-path-ordered, so lookup is a scan (or `std::lower_bound` over the sorted `path`); listing is a contiguous prefix range. Keep it simple and entry-order-faithful — duplicate paths are corruption (Phase 1a guarantees the encoder rejects them), so a linear scan is correct.
```cpp
std::optional<ManifestEntry> Store::lookupPath(const PartManifest & manifest, const String & path) const
{
    for (const auto & entry : manifest.entries)
    {
        if (entry.path == path)
            return entry;
    }
    return std::nullopt;
}

std::vector<ManifestEntry> Store::listDirectory(const PartManifest & manifest, const String & dir_prefix) const
{
    std::vector<ManifestEntry> result;
    for (const auto & entry : manifest.entries)
    {
        if (dir_prefix.empty() || entry.path.starts_with(dir_prefix))
            result.push_back(entry);
    }
    return result;
}
```

- [ ] **Step 3: Write the gtest.** Add to `gtest_cas_store.cpp`. Covers: path hit/miss; directory listing under a prefix; an inline-vs-blob entry distinction (the deliverable's three sub-cases).
```cpp
TEST(CasStore, LookupAndListOverManifestEntries)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const ManifestRef ref{.writer_instance_id = "srv-a", .build_sequence = 9, .manifest_instance_id = u128Of("inst-list")};
    const ManifestId id{.root_namespace = ns, .ref = ref};

    /// Canonical path order: "columns.txt" (inline) < "p.proj/data.bin" (blob) < "data.bin" is NOT here;
    /// keep entries already sorted by the encoder (Phase 1a) — we assert order-faithful behavior.
    std::vector<ManifestEntry> entries;
    entries.push_back(ManifestEntry{.path = "columns.txt", .placement = Placement::Inline,
        .blob_hash = {}, .blob_size = 5, .inline_bytes = "tiny\n"});
    entries.push_back(ManifestEntry{.path = "data.bin", .placement = Placement::Blob,
        .blob_hash = u128Of("hello world"), .blob_size = 11, .inline_bytes = ""});
    entries.push_back(ManifestEntry{.path = "p.proj/data.bin", .placement = Placement::Blob,
        .blob_hash = u128Of("proj"), .blob_size = 4, .inline_bytes = ""});
    writeManifestRaw(*b, layout, id, entries);
    auto m = s->readManifest(id);

    /// path hit: inline vs blob distinguished by placement.
    auto hit_inline = s->lookupPath(m, "columns.txt");
    ASSERT_TRUE(hit_inline.has_value());
    EXPECT_EQ(hit_inline->placement, Placement::Inline);
    EXPECT_EQ(hit_inline->inline_bytes, "tiny\n");

    auto hit_blob = s->lookupPath(m, "data.bin");
    ASSERT_TRUE(hit_blob.has_value());
    EXPECT_EQ(hit_blob->placement, Placement::Blob);
    EXPECT_EQ(hit_blob->blob_size, 11u);

    /// path miss.
    EXPECT_FALSE(s->lookupPath(m, "absent.bin").has_value());

    /// directory listing: only entries under the prefix, in canonical order.
    auto proj = s->listDirectory(m, "p.proj/");
    ASSERT_EQ(proj.size(), 1u);
    EXPECT_EQ(proj[0].path, "p.proj/data.bin");

    auto all = s->listDirectory(m, "");
    EXPECT_EQ(all.size(), 3u);
}
```

- [ ] **Step 4: Build + run.**
```bash
ninja -C build unit_tests_dbms > build/build.log 2>&1 ; echo "ninja exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasStore.LookupAndListOverManifestEntries' > build/test_lookup_list.log 2>&1 ; echo "gtest exit=$?"
```
Expected (analyze logs with a subagent): build succeeds; the test log ends with
```
[  PASSED  ] 1 test.
```

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "CA GC phase1c: lookupPath/listDirectory over canonical-path-ordered manifest entries (no DirectoryIndex yet)"
```

---

### Task 4: `(ManifestId, Token)` decode cache {#task-4-manifestid-token-decode-cache}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp`
- Modify: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces produced:** `manifest_cache` keyed by `(ManifestId, Token)`; cached `readManifest`.

> **Why `(ManifestId, Token)` and not content hash.** The old `tree_cache` was keyed by content hash, so byte-identical trees shared one decode. Each publish now mints a unique `ManifestId`, so there is **no cross-id sharing** — that is the intentional tradeoff from spec §Read Path Scope ("per-instance cache; less sharing"). The `Token` component lets the cache notice an object re-incarnation under the same id (manifests are immutable, but the cache must still fail closed if the backend object's token changes, mirroring the `RootShard` decode cache's token discipline). The HEAD-to-fetch-the-token is the cost; on a token match the immutable decode is reused with no `get` and no re-decode.

- [ ] **Step 1: Replace the tree-cache members with the manifest cache.** In `CasStore.h`, replace the tree decode cache block (`:312-319` — the comment and the `TREE_CACHE_MAX_ENTRIES`/`tree_cache_mutex`/`tree_cache` members) with:
```cpp
    /// Phase 1c manifest decode cache: (ManifestId, Token) -> decoded immutable PartManifest. Part
    /// manifests are immutable single-owner objects, so a token match guarantees identical bytes; the
    /// Token component lets the cache fail closed if the backend object is re-incarnated under the same
    /// id. Unlike the old content-hash tree cache there is NO cross-id sharing — each publish has a
    /// unique ManifestId (spec §Read Path Scope: per-instance cache, less sharing, intentional). The
    /// read path resolves `route` per file, so caching makes a repeated same-part read O(1) decodes.
    /// Bounded (wholesale clear on overflow) to cap memory on a server that reads very many parts.
    struct ManifestCacheKey
    {
        ManifestId manifest_id;
        Token token;
        bool operator==(const ManifestCacheKey &) const = default;
    };
    struct ManifestCacheKeyHash
    {
        size_t operator()(const ManifestCacheKey & k) const;
    };
    static constexpr size_t MANIFEST_CACHE_MAX_ENTRIES = 16384;
    std::mutex manifest_cache_mutex;
    std::unordered_map<ManifestCacheKey, std::shared_ptr<const PartManifest>, ManifestCacheKeyHash> manifest_cache;
```

- [ ] **Step 2: Add the hash and wire the cache into `readManifest`.** In `CasStore.cpp`, add the hash combiner near the top (after the existing `using`/anonymous-namespace block) and thread the cache through `readManifest`: a `head` fetches the current token; on a `(id, token)` hit return the cached immutable decode; otherwise `get` + validate (Task 2 logic) + populate. The validation (`refMatchesBody`/`manifestNamespaceMatches`, missing-body fail-closed) is unchanged — only the fetch is now token-gated.
```cpp
size_t Store::ManifestCacheKeyHash::operator()(const ManifestCacheKey & k) const
{
    /// Combine the manifest-id hash with the token hash. Phase 1a emits `std::hash<ManifestId>`
    /// (alongside `operator==`/`operator<`), so this `std::unordered_map` keying compiles directly
    /// against the landed surface. The token is part of the key so a re-incarnation under the same id misses.
    const size_t h1 = std::hash<ManifestId>{}(k.manifest_id);
    const size_t h2 = std::hash<Token>{}(k.token);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
}
```
Then, at the top of `readManifest` (after computing `key`), HEAD for the token and consult the cache:
```cpp
    const String key = pool_layout.manifestKey(id);
    const HeadResult head = pool_backend->head(key);
    if (!head.exists)
    {
        /// ... existing FILE_DOESNT_EXIST fail-closed branch (unchanged) ...
    }
    {
        std::lock_guard lock(manifest_cache_mutex);
        auto it = manifest_cache.find(ManifestCacheKey{.manifest_id = id, .token = head.token});
        if (it != manifest_cache.end())
            return *it->second;
    }
    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "manifest at {} vanished between head and get — INV-NO-DANGLE", key);
    /// ... decode + refMatchesBody + manifestNamespaceMatches (unchanged Task 2 validation) ...
    auto decoded = std::make_shared<const PartManifest>(std::move(body));
    {
        std::lock_guard lock(manifest_cache_mutex);
        if (manifest_cache.size() >= MANIFEST_CACHE_MAX_ENTRIES)
            manifest_cache.clear();
        manifest_cache[ManifestCacheKey{.manifest_id = id, .token = head.token}] = decoded;
    }
    return *decoded;
```
(Keep the `ReadMissing`/`CorruptDecode` event emissions from Task 2 in their branches. The earlier Task 2 `get`-then-validate body is refactored, not duplicated — the missing-body branch now keys off `head.exists`.)

- [ ] **Step 3: Write the gtest** (uses `CountingBackend` to assert the token-gated cache behavior). Covers: hit (second read does no second `get`); miss-on-token-change (a re-incarnation forces a fresh `get`); no cross-id sharing (a different `ManifestId` over identical bytes does not hit the first id's entry).
```cpp
TEST(CasStore, ManifestCacheIsKeyedByIdAndToken)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const ManifestRef ref{.writer_instance_id = "srv-a", .build_sequence = 1, .manifest_instance_id = u128Of("inst-cache")};
    const ManifestId id{.root_namespace = ns, .ref = ref};
    std::vector<ManifestEntry> entries;
    entries.push_back(ManifestEntry{.path = "a", .placement = Placement::Inline, .blob_hash = {}, .blob_size = 1, .inline_bytes = "x"});
    writeManifestRaw(*b, layout, id, entries);
    const String key = layout.manifestKey(id);

    /// First read populates; second read is a (id, token) HIT — no second get on the manifest key.
    (void)s->readManifest(id);
    b->resetCounts();
    (void)s->readManifest(id);
    EXPECT_EQ(b->getCount(key), 0u);   /// served from cache; HEAD may run, get must not

    /// Re-incarnate the object under the SAME id (new token) ⇒ MISS ⇒ a fresh get.
    DB::Cas::tests::displaceObjectToken(*b, key, DB::Cas::ObjectKind::Tree);
    b->resetCounts();
    (void)s->readManifest(id);
    EXPECT_EQ(b->getCount(key), 1u);   /// token changed ⇒ cache missed ⇒ re-fetched

    /// No cross-id sharing: a DIFFERENT ManifestId (byte-identical entries) does not reuse id's entry.
    const ManifestRef ref2{.writer_instance_id = "srv-a", .build_sequence = 1, .manifest_instance_id = u128Of("inst-cache-2")};
    const ManifestId id2{.root_namespace = ns, .ref = ref2};
    writeManifestRaw(*b, layout, id2, entries);
    const String key2 = layout.manifestKey(id2);
    b->resetCounts();
    (void)s->readManifest(id2);
    EXPECT_EQ(b->getCount(key2), 1u);   /// distinct id ⇒ its own first-time get, never a hit on id
}
```
(If the wiring's `ObjectKind` for manifests differs from `Tree`, pass the matching kind to `displaceObjectToken`; the helper only needs to re-encode the envelope and mint a new token.)

- [ ] **Step 4: Build + run.**
```bash
ninja -C build unit_tests_dbms > build/build.log 2>&1 ; echo "ninja exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasStore.ManifestCacheIsKeyedByIdAndToken' > build/test_manifest_cache.log 2>&1 ; echo "gtest exit=$?"
```
Expected (analyze logs with a subagent): build succeeds; the test log ends with
```
[  PASSED  ] 1 test.
```

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "CA GC phase1c: manifest decode cache keyed by (ManifestId, Token), no cross-id sharing"
```

---

### Task 5: Wire `ContentAddressedMetadataStorage` read helpers to `readManifest` / `lookupPath` {#task-5-wire-contentaddressedmetadatastorage-read-helpers-to-readmanifest-lookuppath}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Disks/tests/gtest_ca_wiring.cpp`

**Interfaces produced:** `resolveRouted` returning `std::pair<Cas::Resolved, Cas::PartManifest>`; read helpers over `lookupPath` / `listDirectory`.

- [ ] **Step 1: Change `resolveRouted`'s return type.** In `ContentAddressedMetadataStorage.h`, change the declaration of `resolveRouted` from `std::optional<std::pair<Cas::Resolved, std::vector<Cas::TreeEntry>>>` to `std::optional<std::pair<Cas::Resolved, Cas::PartManifest>>`. In the `.cpp` (`:488-496`) update the definition:
```cpp
std::optional<std::pair<Cas::Resolved, Cas::PartManifest>>
ContentAddressedMetadataStorage::resolveRouted(const Route & r) const
{
    auto resolved = store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true);
    if (!resolved)
        return std::nullopt;
    /// A live ref to a missing/corrupt manifest throws (INV-NO-DANGLE surfaced, never substituted).
    return std::make_pair(*resolved, store()->readManifest(resolved->manifest_id));
}
```

- [ ] **Step 2: Rewire the entry-iterating read helpers.** Every site that iterated `rt->second` as a `std::vector<Cas::TreeEntry>` matching `entry.name` now uses `lookupPath` / `listDirectory` on the `Cas::PartManifest`, matching `entry.path`. The minimal, faithful changes:
  - `existsFile` (`:533-539`): replace the `for (const auto & entry : rt->second) if (entry.name == r->file)` loop with `return store()->lookupPath(rt->second, r->file).has_value();`.
  - `getStorageObjects` / `getFileSize` / `getBlobViewPlan` sites (around `:866`, `:952`, `:989`): replace the per-file linear search over `rt->second` and the `store()->locate(entry)` call with `auto entry = store()->lookupPath(rt->second, r->file); if (!entry) ...; const auto location = store()->locate(*entry);` — `locate` now takes a `ManifestEntry`.
  - directory-listing sites that collapsed `entry.name` to first path segments now collapse `entry.path` from `listDirectory(rt->second, dir_prefix)` (same first-segment collapse the wiring already does for tree entries).
  Keep `entry.placement` / `inline_bytes` reads identical — `ManifestEntry` has the same `placement` enum and `inline_bytes` field as `TreeEntry`, only `name`→`path` and `file_hash`/`file_size`→`blob_hash`/`blob_size`.

- [ ] **Step 3: Make `locate` accept a `ManifestEntry`.** `Store::locate` currently takes a `TreeEntry`. Change its signature to `BlobLocation locate(const ManifestEntry & entry) const` and update the body to read `entry.blob_hash` / `entry.blob_size` (it already only handles `Placement::Blob`, throwing for `Inline`; there is no `Subtree` placement on a part manifest, so the `Subtree` arm becomes a `Placement::Inline`-only fallthrough). Update the declaration in `CasStore.h` (`:120`).

- [ ] **Step 4: Run the wiring tests (failing first if the helpers were missed).** The `CaWiring*` suite exercises the metadata storage black-box (`existsFile`, `listDirectory`, `getStorageObjects`, `getFileSize`). It must keep passing once the wiring is rewired. Build + run:
```bash
ninja -C build unit_tests_dbms > build/build.log 2>&1 ; echo "ninja exit=$?"
build/src/unit_tests_dbms --gtest_filter='CaWiring*' > build/test_ca_wiring.log 2>&1 ; echo "gtest exit=$?"
```
Expected (analyze logs with a subagent): build succeeds; the test log ends with (the count is whatever the suite holds — confirm no failures):
```
[  PASSED  ] <N> tests.
```
If a `CaWiring*` test fixture publishes parts via a tree-era helper (`publishWiredPart` building `RefPayload`/tree), update that fixture to publish a `RootRef` + a `writeManifestRaw` body — the same black-box assertions then hold over the manifest read path.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "CA GC phase1c: wire ContentAddressedMetadataStorage read helpers to readManifest/lookupPath/listDirectory"
```

---

### Task 6: Build + full `Cas*`/`Ca*` gtest sweep {#task-6-build-full-cas-ca-gtest-sweep}

**Files:** none new (verification + phase-exit commit).

- [ ] **Step 1: Confirm no `TreeId` / `readTree` / `CasTreeCodec` survives on the read path.** Grep — these must be gone from `CasStore.*` and the metadata-storage read helpers (Phase 1d deletes `CasTreeCodec` wholesale; this phase must not still reference it from the read path):
```bash
grep -rn "readTree\|TreeId\|CasTreeCodec\|tree_cache\|tree_id" \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
```
Expected: no output (an empty grep). If anything prints, it is a leftover read-path reference — fix it before proceeding.

- [ ] **Step 2: Clean build of the unit test binary.**
```bash
ninja -C build unit_tests_dbms > build/build.log 2>&1 ; echo "ninja exit=$?"
```
Expected (analyze `build/build.log` with a subagent and return only a concise summary): `ninja exit=0`, no errors.

- [ ] **Step 3: Run the full `Cas*` + `Ca*` sweep.**
```bash
build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_cas_ca_sweep.log 2>&1 ; echo "gtest exit=$?"
```
Expected (analyze `build/test_cas_ca_sweep.log` with a subagent): `gtest exit=0`; the log ends with
```
[  PASSED  ] <N> tests.
```
and no `[  FAILED  ]` lines. (`<N>` is the full Cas/Ca count including the four new `CasStore` tests from Tasks 1–4 and the rewired `CaWiring*` suite.) If any test fails, use superpowers:systematic-debugging before continuing — do not paper over a failure.

- [ ] **Step 4: Commit the phase-exit marker.**
```bash
git commit --allow-empty -m "CA GC phase1c: read path on part manifests — full Cas*/Ca* sweep green"
```

---

## Self-Review {#self-review}

- **Contract coverage:** every Phase 1c EMIT in the canonical contract maps to a task — `Resolved.manifest_id` + `resolveRef` (Task 1), `readManifest` + `refMatchesBody`/`manifestNamespaceMatches` + fail-closed missing committed body (Task 2), `lookupPath`/`listDirectory` over canonical-path-ordered entries with no `DirectoryIndex` (Task 3), `(ManifestId, Token)` cache with no cross-id sharing (Task 4), wiring rewire (Task 5). ✓
- **Spec sections honored:** §Read Path Scope (resolve→`ManifestId`, `readManifest` via `CasLayout`, `refMatchesBody`/`manifestNamespaceMatches`, path lookup + directory listing, bounded cache, fail-closed on missing committed manifest) and the intentional tradeoff (less cache sharing) are stated in the Architecture, the OQ block, and Task 4's "Why" note. §Object Identity And Ownership's two trustworthiness checks are enforced fail-closed in `readManifest`. §Core Principle (only blobs content-addressed; manifests per-instance) is reflected in keying the cache by id, not hash. ✓
- **TDD + bite-sized + commits:** each task is gtest-first → build/run → commit; every gtest step shows the `TEST(CasStore, …)` body, the exact `build/src/unit_tests_dbms --gtest_filter=...` command, and the expected `[ PASSED ]`. Run steps give the exact command + expected output; build/test logs are redirected and analyzed by a subagent per the constraints. ✓
- **No placeholders:** real C++ in every code step (header edits, `readManifest`/`lookupPath`/`listDirectory`/cache bodies, wiring edits) and a real test body per task. The only deferrals are explicit Phase 1a/1b helper-spelling adaptations (called out in the ground-truth note) and the test-fixture helpers (`writeManifestRaw`, `RootRef` `publishRaw` overload), which are specified as "mirror the existing `writeTreeRaw`/`publishRaw`" — data, not vague instruction. ✓
- **Only contract type names:** `ManifestRef`, `ManifestId`, `ManifestEntry`, `PartManifest`, `decodePartManifest`, `refMatchesBody`, `manifestNamespaceMatches`, `CasLayout::manifestKey`, `RootRef`, `Resolved`, `readManifest`, `lookupPath`, `listDirectory`. No `TreeId`/`readTree`/`CasTreeCodec` in the EMITTED surface; Task 6 grep enforces their absence on the read path. ✓
- **Allman braces** throughout the shown C++. ✓
- **Spec gap surfaced:** the spec's read path is silent on whether `readManifest` should HEAD-then-cache or read-then-cache. Task 4 chooses HEAD-gated `(ManifestId, Token)` caching to mirror the existing `RootShard` decode-cache token discipline and to keep the cache fail-closed under object re-incarnation; if Phase 1a/1b made manifests strictly create-once (no re-incarnation under a live id), the HEAD could be dropped and the cache keyed by `ManifestId` alone — a one-line simplification left to the implementer's read of the landed Phase 1a invariants.

**Gate reminder:** this phase's code may not start until Phase 0's model suite is GREEN, and Phase 1c depends on Phase 1a (codecs + `manifestKey`) and coordinates with Phase 1b's `RootRef`.
