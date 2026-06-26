---
description: "Build, precommit, atomic promote, and root-journal owner transitions for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 1b (write path)"
sidebar_position: 4
slug: /superpowers/plans/2026-06-26-cas-gc-phase1b-build-precommit-promote
title: "Phase 1b — Build / Precommit / Promote (write path) — Implementation Plan"
doc_type: reference
---

# Phase 1b — Build / Precommit / Promote (write path) — Implementation Plan {#phase-1b-build-precommit-promote-write-path-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax — each step is one 2–5 min action; each task ends with a commit. Read `2026-06-26-cas-gc-redesign-overview.md` first, then this. **Gate:** Phase 0 must be GREEN (its `_RESULTS.md` ledger marks the R0 suite GREEN) before any code task here begins. **Depends on:** Phase 1a (consume its emitted types verbatim — see [Canonical Contract](#canonical-contract)).

**Goal:** Convert the CA write path from the content-addressed-tree model to root-local single-owner part manifests. `Build::stageTree`→`Build::stageManifest` mints a `ManifestId` and stream-writes the body under `_manifests`; `precommitAdd` records a `PrecommitTransition` in the **target** root shard (no `_precommits` namespace); `promote` is one atomic single-shard CAS owner move with fail-closed revalidation; `republishRef` publishes a fresh destination `PartManifest` over the same blob hashes then drops the source; the writer best-effort cleans its own `_manifests` debris on `abandon`. The root journal carries compact `OwnerTransition`/`PrecommitTransition`/`PromotePrecommit` records — never transitive closures.

**Architecture:** Only **blobs** stay content-addressed. A part is one immutable, single-owner, namespace-qualified `ManifestId = (root_namespace_id, ManifestRef)` whose body lists every path, inline payload, and blob reference. The root journal carries owner transitions (old/new `ManifestRef`), not `ClosureNode` closures. Promotion is an owner move from `precommit(build_id)` to `ref(final_ref_name)` in one root-shard CAS. `JournalRecord`/`ClosureNode`/`buildStagedClosure` are removed; the separate `<server_hex>/_precommits` namespace disappears (the precommit lives in the target root shard).

**Tech Stack:** C++ (ClickHouse coding standards, Allman braces); Protobuf for the control-plane root journal (`RootShardManifest`, magic via `FormatId::Manifest`); gtest unit oracles against `InMemoryBackend`. No TLA+ in this phase (Phase 0 is the gate).

## Global Constraints {#global-constraints}

*Every task in this plan implicitly includes this section (copied verbatim from the overview).*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in a phase may begin until that phase's TLA+ gate is green (see [Execution Model & Gates](#execution-model--gates)).
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
- Build into a `build_*` directory (e.g. `build`, `build_debug`, `build_asan`). Always redirect ninja output to `<build_dir>/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'`.

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- **OQ6 (sweep-eligibility encoding):** `writer_instance_id` is a `String` `"<server_id_hex>:<process_epoch>"`, minted from `server_id` plus a durable monotone `process_epoch` (`Store::epoch`, the per-process-incarnation counter already minted at open). This is the writer-incarnation identity stamped into every staged manifest body and into the `_manifests/<writer_instance_id>/<build_sequence>/` key prefix. The durable watermark records, per `writer_instance_id`, the `(server_id, process_epoch, retired)` tuple that sweep-eligibility consumes — read from that durable tuple, never parsed-only out of the string, and never a frozen-seq / judged-dead heuristic.
- **OQ7 (backpressure thresholds):** enforced fail-closed *inside* `stageManifest`, *before* the body write returns and therefore before any owner transition is published: `manifest_entries ≤ 1048576`; `manifest_encoded_bytes ≤ 256 MiB`; `manifest_inline_bytes_total ≤ 16 MiB`; `largest_inline_entry_bytes ≤ 1 MiB`. (The `blob_delta_bytes_per_generation` soft-cap is a GC-side concern — Phase 1d, not here.)
- **OQ1/OQ2/OQ3 (manifest body fields, `_manifests` placement, internal entry indexing):** owned by Phase 1a (`CasManifestId.h`, `CasManifestCodec.*`, `CasRunFile.*`, `CasLayout::manifestKey`). Phase 1b consumes them; it does not redefine them.

## Canonical Contract {#canonical-contract}

Phase 1a **emits** these (consume verbatim — do not redefine; only their type names may appear in this plan):

```cpp
struct ManifestRef { String writer_instance_id; uint64_t build_sequence; UInt128 manifest_instance_id; };
struct ManifestId  { String root_namespace; ManifestRef ref; };   // (root_namespace_id, ManifestRef)
enum class EntryPlacement : uint8_t { Inline = 1, Blob = 2 };
struct ManifestEntry { String path; EntryPlacement placement; UInt128 blob_hash; uint64_t blob_size; String inline_bytes; };
struct PartManifest { ManifestRef ref; String root_namespace_id; UInt128 payload_digest; std::vector<ManifestEntry> entries; };
String       encodePartManifest(const PartManifest &);
PartManifest decodePartManifest(std::string_view);
bool refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body);          // RefMatchesBody
bool manifestNamespaceMatches(const String & owning_ns, const PartManifest & body);        // ManifestNamespaceMatches
// CasLayout::manifestKey(const ManifestId &)  ->  <pool>/roots/<ns>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto
```

Phase 1b **EMITS** these (this plan creates them, in `CasRootShardCodec.h`/`.cpp` and `CasBuild.h`/`.cpp`):

```cpp
// CasRootShardCodec.h — replaces RefPayload; JournalRecord + ClosureNode are REMOVED.
struct RootRef { String ref_name; ManifestRef manifest_ref; std::map<String,String> mutable_files; uint64_t published_at_ms; };
struct OwnerTransition     { uint64_t transition_version; String ref_name; std::optional<ManifestRef> old_manifest; std::optional<ManifestRef> new_manifest; };
struct PrecommitTransition { uint64_t transition_version; UInt128 build_id; String final_ref_name; std::optional<ManifestRef> old_manifest; std::optional<ManifestRef> new_manifest; };
struct PromotePrecommit    { uint64_t transition_version; UInt128 build_id; String final_ref_name; ManifestRef manifest; };
struct RootShard { uint64_t shard_version; uint64_t fence_round; std::map<String,RootRef> refs; std::vector<OwnerTransition> transitions; std::vector<PrecommitTransition> precommits; std::vector<PromotePrecommit> promotions; };
```

```cpp
// CasBuild.h — stageTree/precommit/publish are replaced; buildStagedClosure is removed.
ManifestId stageManifest(std::vector<ManifestEntry> entries);
void       precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id);
void       promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 build_id, const ManifestId & id);
```

Semantics required of the emitted types (spec §Root Journal Format / §Build And Precommit Protocol):
- `RootRef` carries the committed `ManifestRef` plus mutable per-ref payload; `root_namespace_id` is **not** stored in the ref (it comes from the owning root context).
- An `OwnerTransition`/`PrecommitTransition` with `old=none,new=T` is a create; `old=T,new=none` is a drop/abandon; `old=T_old,new=T_new` is a repoint.
- `PromotePrecommit` carries the single manifest whose owner moves from `precommit(build_id)` to `ref(final_ref_name)` — no blob-edge change on the normal (already-activated) path.

## Spec invariants this phase upholds (task map) {#spec-invariants-this-phase-upholds-task-map}

| Invariant (spec §Safety Invariants) | Upheld by task |
|---|---|
| `SingleManifestOwner` | T3 (`PrecommitTransition` in target shard), T5 (atomic single-shard `promote`), T6 (`republishRef` = fresh dst + src drop, no shared/moved manifest) |
| `NoManifestIdReuse` | T2 (random `manifest_instance_id` per `stageManifest`) |
| `CommittedManifestBodyRequired` / `NoCommittedDangle` | T5 (fail-closed revalidation: missing body/blob/condemned ⇒ `ABORTED`) |
| `PrecommitMayReferenceMissingManifest` / `PrecommitMayReferenceMissingBlob` | T3 + T4 (precommit CAS needs no body `HEAD`; blobs upload *after* `precommitAdd`) |
| `RefMatchesBody` / `ManifestNamespaceMatches` | T5 (revalidation streams + validates the body before the owner move) |
| `MutablePayloadNotReachability` | T1 (a mutable-only update is a root-shard CAS changing `RootRef.mutable_files` only — no `OwnerTransition`, no blob delta, no `ManifestId` change) |
| `ManifestActivationMatchesEdges` | T5 (missing-body activation ⇒ promotion emits committed `+` edges; already-activated ⇒ pure move) |
| `OrphanManifestDebrisDrains` (liveness) | T7 (writer best-effort `_manifests` debris cleanup on `abandon`) |
| backpressure caps (OQ7) | T2 (fail-closed before the body write returns) |

## File Structure {#file-structure}

- Modify: `CasRootShardCodec.h` / `CasRootShardCodec.cpp` — new types + protobuf codec; remove `JournalRecord`/`ClosureNode`. (T1)
- Modify: `Proto/cas_format.proto` — `RootShardManifest` message: replace `RefPayload`/`JournalRecord`/`ClosureNode` fields with `RootRef`/`OwnerTransition`/`PrecommitTransition`/`PromotePrecommit`. (T1)
- Modify: `CasBuild.h` / `CasBuild.cpp` — `stageManifest`, `precommitAdd`, `promote`; remove `stageTree`/`uploadStagedTree`/`putTree`/`precommit`/`publish`/`buildStagedClosure`/`precommitNs`/`buildRef`/`buildShard`; writer debris cleanup in `abandon`. (T2–T5, T7)
- Modify: `ContentAddressedTransaction.cpp` — `republishRef` (`:134`) → fresh dst `PartManifest` + src drop; call sites `:871` (RENAME loop) and `:1025` (part-dir move). (T6)
- Test: `src/Disks/tests/gtest_cas_build.cpp`, `gtest_cas_build_root_dangle.cpp`, `gtest_ca_transaction.cpp`, helpers `src/Disks/tests/cas_test_helpers.h` (extend `publishRaw`/add manifest-publish helper as needed). (all tasks)

## Interfaces consumed from current code (ground truth) {#interfaces-consumed-from-current-code-ground-truth}

- `Store::mutateShard(ns, shard, mutate, out_committed_version=nullptr)` (`CasStore.h:241`) — the verified CAS loop (re-read inside loop, size guard, `casPut`, bounded retry); `mutate` runs on the freshly-read `RootShard` each attempt. Reused by `precommitAdd`/`promote`/`republishRef`; never duplicated.
- `Store::shardOf(ref_name)` (`CasStore.h:171`) = `CityHash64(ref_name) % root_shards` — the shard a ref routes to. The precommit and the future committed ref share `shardOf(final_ref_name)`, so promotion is one shard CAS.
- `Store::ensureRegistered(ns)` (`CasStore.h:181`) — W-REGISTER; returns the registry fence floor.
- `Store::startBuild(BuildInfo)` (`CasStore.h:115`); `Store::resolveRef(ns, ref, allow_stale=false)`/`Resolved` (`CasStore.h:118`); `Store::dropRef(ns, ref)` (`CasStore.h:133`); `Store::layout()` / `Store::backend()` / `Store::epoch()`.
- `Backend::putIfAbsentStream(key, meta={})` + `WriteSink` (`CasBackend.h:135,93`) — streaming conditional create; `finalize`→`PutResult{Done|PreconditionFailed, token}`; `cancel`/destruction abandons the upload. `Backend::head` / `Backend::casPut(key,bytes,expected)` / `Backend::deleteExact(key,token)`.
- `Build::uploadFromSource(kind,hash,key,bytes)` (`CasBuild.cpp:277`) — INV-1 revival-from-source primitive (never `GET`s the dying object); reused by blob upload (T4).
- `FormatId::Manifest` + `magicFor(FormatId)` (`CasFormat.h:22,35`) — the root-journal framing magic, unchanged (`encodeRootShard` already calls `set_magic(magicFor(FormatId::Manifest))` at `CasRootShardCodec.cpp:105`).

---

## Tasks {#tasks}

### Task 1: `CasRootShardCodec` new types + protobuf codec; remove `JournalRecord`/`ClosureNode` {#task-1-casrootshardcodec-new-types-protobuf-codec-remove-journalrecord-closurenode}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Proto/cas_format.proto`
- Modify: `src/Disks/tests/gtest_cas_root_shard_codec.cpp` (or the codec round-trip gtest; create a `CasRootShardCodec` test group there)

**Interfaces produced:** `RootRef`, `OwnerTransition`, `PrecommitTransition`, `PromotePrecommit`, `RootShard` (rewritten); `encodeRootShard`/`decodeRootShard` over the new shape; `JournalRecord`/`ClosureNode` removed.

**Upholds:** `MutablePayloadNotReachability` (mutable payload is a `RootRef` field, separate from owner transitions); compact owner transitions (no transitive closure).

- [ ] **Step 1: Replace the header types.** In `CasRootShardCodec.h`, delete `struct ClosureNode`, `struct RefPayload`, and `struct JournalRecord`. Add (note `#include` for `CasManifestId.h` from Phase 1a; drop the `CasTreeCodec.h` include):

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// The current committed ref payload in the root journal. Carries the committed `ManifestRef` plus the
/// mutable per-ref files (txn_version.txt, metadata_version.txt, ...). `root_namespace_id` is NOT
/// stored here — it comes from the owning root context (spec §Root Journal Format).
struct RootRef
{
    String ref_name;
    ManifestRef manifest_ref;
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
    bool operator==(const RootRef &) const = default;
};

/// Atomic old/new committed-owner change in one root-journal decision. old=none,new=T: create;
/// old=T,new=none: drop; old=T_old,new=T_new: repoint. Carries full `ManifestRef`s, never a bare nonce.
struct OwnerTransition
{
    uint64_t transition_version = 0;
    String ref_name;
    std::optional<ManifestRef> old_manifest;
    std::optional<ManifestRef> new_manifest;
    bool operator==(const OwnerTransition &) const = default;
};

/// Build-intent owner change for a precommit, recorded in the TARGET root shard (no `_precommits`
/// namespace). old=none,new=T: PrecommitAdd; old=T,new=none: abandon/reclaim.
struct PrecommitTransition
{
    uint64_t transition_version = 0;
    UInt128 build_id{};
    String final_ref_name;
    std::optional<ManifestRef> old_manifest;
    std::optional<ManifestRef> new_manifest;
    bool operator==(const PrecommitTransition &) const = default;
};

/// Atomic owner move from precommit(build_id) to ref(final_ref_name). Same manifest; owner only moves.
struct PromotePrecommit
{
    uint64_t transition_version = 0;
    UInt128 build_id{};
    String final_ref_name;
    ManifestRef manifest;
    bool operator==(const PromotePrecommit &) const = default;
};

struct RootShard
{
    uint64_t shard_version = 0;
    uint64_t fence_round = 0;
    std::map<String, RootRef> refs;                  /// std::map keeps refs in canonical name order
    std::vector<OwnerTransition> transitions;        /// committed owner transitions, insertion order
    std::vector<PrecommitTransition> precommits;     /// precommit intents in this shard, insertion order
    std::vector<PromotePrecommit> promotions;        /// promote-precommit owner moves, insertion order (GC-foldable)
    bool operator==(const RootShard &) const = default;
};

String encodeRootShard(const RootShard & root);
RootShard decodeRootShard(std::string_view data);

}
```

- [ ] **Step 2: Rewrite the protobuf message.** In `Proto/cas_format.proto`, in `message RootShardManifest`: replace the `RefPayload`/`JournalRecord`/`ClosureNode` sub-messages and fields with `RootRef`, `OwnerTransition`, `PrecommitTransition`, `PromotePrecommit`. Make every `ManifestRef` a nested message `{ string writer_instance_id = 1; uint64 build_sequence = 2; bytes manifest_instance_id = 3; }` (16-byte fixed for the `UInt128`); make `old_manifest`/`new_manifest` `optional ManifestRef` (proto3 explicit-presence) so absence is distinguishable from a zero ref. Keep the message name `RootShardManifest` and the magic via `FormatId::Manifest` (the redesign keeps the on-wire class id; only its fields change — CA is pre-release, so no compat path).

- [ ] **Step 3: Rewrite the codec body.** In `CasRootShardCodec.cpp`, update `encodeRootShard`/`decodeRootShard` to map the new structs to/from the rewritten protobuf. Keep `set_magic(magicFor(FormatId::Manifest))` and `currentWriterVersion()` framing (`.cpp:105-106`). Encode `refs` in name-sorted order (`std::map` gives it), `transitions`/`precommits`/`promotions` in insertion order. Encode each `ManifestRef.manifest_instance_id` as exactly 16 bytes. Fail-closed decode: a `manifest_instance_id` whose length ≠ 16, a missing required field, a `transition` with neither `old_manifest` nor `new_manifest` set, or a bad envelope ⇒ `CORRUPTED_DATA`.

- [ ] **Step 4: Write the codec gtest.** In the codec gtest, add a `CasRootShardCodec` group:

```cpp
TEST(CasRootShardCodec, RoundTripWithTransitionsAndPrecommits)
{
    RootShard r;
    r.shard_version = 7;
    r.fence_round = 3;
    ManifestRef mr{"srv-a:42", 1042, UInt128(0xABCDEF)};
    r.refs["all_1_1_0"] = RootRef{"all_1_1_0", mr, {{"txn_version.txt", "5"}}, 1700000000000ULL};
    r.transitions.push_back(OwnerTransition{8, "all_1_1_0", std::nullopt, mr});
    r.precommits.push_back(PrecommitTransition{8, UInt128(0x5678), "all_1_1_0", std::nullopt, mr});
    r.promotions.push_back(PromotePrecommit{9, UInt128(0x5678), "all_1_1_0", mr});

    const String bytes = encodeRootShard(r);
    const RootShard back = decodeRootShard(bytes);
    EXPECT_EQ(back, r);
    /// Byte-equality: deterministic encoder ⇒ re-encode is byte-identical (resume/adoption).
    EXPECT_EQ(encodeRootShard(back), bytes);
}

TEST(CasRootShardCodec, OptionalManifestAbsenceIsDistinguished)
{
    RootShard r;
    r.transitions.push_back(OwnerTransition{1, "p", ManifestRef{"w", 1, UInt128(9)}, std::nullopt});
    const RootShard back = decodeRootShard(encodeRootShard(r));
    EXPECT_FALSE(back.transitions.at(0).new_manifest.has_value());
    EXPECT_TRUE(back.transitions.at(0).old_manifest.has_value());
}
```

- [ ] **Step 5: Compile the codec TU only** to catch the type/proto churn early. Run (redirect to log; analyze with a subagent):

```bash
ninja -C build CasRootShardCodec.cpp.o > build/build_phase1b_t1.log 2>&1
```

Expected: the codec object compiles. (Downstream TUs still referencing `RefPayload`/`JournalRecord` will fail to compile until later tasks; that is expected — do not fix them here.)

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Proto/cas_format.proto \
        src/Disks/tests/gtest_cas_root_shard_codec.cpp
git commit -m "CA GC phase1b: root-journal owner-transition types + codec (remove JournalRecord/ClosureNode)"
```

---

### Task 2: `Build::stageManifest` — mint identity, stream-write body, caps fail-closed {#task-2-build-stagemanifest-mint-identity-stream-write-body-caps-fail-closed}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp`
- Modify: `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces produced:** `ManifestId Build::stageManifest(std::vector<ManifestEntry>)`; `writer_instance_id` derivation; the OQ7 caps. (`stageTree`/`uploadStagedTree`/`putTree`/`buildStagedClosure`/`retained_trees`/`source_tree_cache` removed.)

**Upholds:** `NoManifestIdReuse` (random `manifest_instance_id`); backpressure caps (OQ7, fail-closed before the body write returns); `PrecommitMayReferenceMissingManifest`/`Blob` (body is written, but safety does not depend on a pre-CAS body `HEAD`).

- [ ] **Step 1: Declare the writer identity + caps in the header.** In `CasBuild.h`, drop the `#include <...CasTreeCodec.h>`, add `#include <...CasManifestId.h>` and `#include <...CasManifestCodec.h>`. Replace the `stageTree`/`uploadStagedTree`/`putTree` declarations and the `buildStagedClosure`/`retained_trees`/`source_tree_cache` members with:

```cpp
    /// Mint a root-local part ManifestId, stream-write its body under
    /// `_manifests/<writer_instance_id>/<build_sequence>/...` via putIfAbsentStream (NO preliminary HEAD —
    /// the manifest_instance_id is random). Enforces the OQ7 caps fail-closed BEFORE the body write
    /// returns (and therefore before any owner transition is published). The body is not retained after a
    /// successful write; on retry the caller re-stages from source. NoManifestIdReuse: a fresh random
    /// manifest_instance_id per call.
    ManifestId stageManifest(std::vector<ManifestEntry> entries);
```
```cpp
    /// "<stable_server_id>:<process_epoch>" — the writer-incarnation id (OQ6). Stamped into the manifest
    /// body `ref` and into the `_manifests/<writer_instance_id>/...` key prefix so a new process epoch
    /// never reuses a build prefix.
    String writerInstanceId() const;
```

- [ ] **Step 2: Add the caps as named constants** near the top of `CasBuild.cpp` (anonymous namespace):

```cpp
/// OQ7 backpressure caps, enforced fail-closed in stageManifest BEFORE the body write returns.
constexpr uint64_t kMaxManifestEntries = 1048576;
constexpr uint64_t kMaxManifestEncodedBytes = 256ULL << 20;        /// 256 MiB
constexpr uint64_t kMaxManifestInlineBytesTotal = 16ULL << 20;     /// 16 MiB
constexpr uint64_t kMaxLargestInlineEntryBytes = 1ULL << 20;       /// 1 MiB
```

- [ ] **Step 3: Implement `writerInstanceId` and `stageManifest`.** Replace `Build::stageTree`/`uploadStagedTree`/`putTree`/`buildStagedClosure` in `CasBuild.cpp` with:

```cpp
String Build::writerInstanceId() const
{
    /// OQ6: stable server id + durable process epoch. A new process incarnation gets a new epoch, so it
    /// cannot reuse a prior incarnation's `_manifests/<writer_instance_id>/...` build prefix.
    return u128ToHex(store->poolConfig().server_id) + ":" + std::to_string(epoch);
}

ManifestId Build::stageManifest(std::vector<ManifestEntry> entries)
{
    requireAlive();

    /// Fail-closed caps (OQ7) — checked BEFORE the body write so no owner transition can ever name a
    /// manifest that breaches a cap. Inline payload is read on every part-open and every owner
    /// transition, so cap the total, not only per-entry.
    if (entries.size() > kMaxManifestEntries)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: {} entries exceeds cap {}", entries.size(), kMaxManifestEntries);
    uint64_t inline_total = 0;
    for (const ManifestEntry & e : entries)
    {
        if (e.placement == EntryPlacement::Inline)
        {
            if (e.inline_bytes.size() > kMaxLargestInlineEntryBytes)
                throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                    "stageManifest: inline entry '{}' of {} bytes exceeds cap {}",
                    e.path, e.inline_bytes.size(), kMaxLargestInlineEntryBytes);
            inline_total += e.inline_bytes.size();
        }
    }
    if (inline_total > kMaxManifestInlineBytesTotal)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: total inline {} bytes exceeds cap {}", inline_total, kMaxManifestInlineBytesTotal);

    /// Mint the identity. manifest_instance_id is random (NoManifestIdReuse) and never derived from
    /// payload. With writer_instance_id + build_seq it forms the ManifestRef; with the owning namespace
    /// it forms the ManifestId.
    const ManifestRef ref{writerInstanceId(), build_seq, mintU128()};

    /// Build the body. payload_digest is integrity/debug only — never a key, never dedup, never
    /// in-degree (spec §Part Manifest Reference And Identity). The owning namespace is the build's
    /// intended ref namespace; the body repeats its own ref + namespace for fail-closed RefMatchesBody /
    /// ManifestNamespaceMatches at read/fold/promote time.
    const RootNamespace owning_ns = manifestNamespace();
    PartManifest body;
    body.ref = ref;
    body.root_namespace_id = owning_ns.string();
    body.entries = std::move(entries);
    body.payload_digest = computePayloadDigest(body);   /// from CasManifestCodec (Phase 1a)
    const String encoded = encodePartManifest(body);
    if (encoded.size() > kMaxManifestEncodedBytes)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: encoded manifest {} bytes exceeds cap {}", encoded.size(), kMaxManifestEncodedBytes);

    const ManifestId id{owning_ns.string(), ref};
    const String key = store->layout().manifestKey(id);

    /// Stream-write the body — NO preliminary HEAD (the instance id is random). A PreconditionFailed
    /// would mean a 128-bit collision: fail closed before any root transition becomes visible.
    WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
    writeString(encoded, sink->buffer());
    const PutResult res = sink->finalize();
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "stageManifest: manifest_instance_id collision at {} (PreconditionFailed) — failing closed", key);

    return id;
}
```
(Add `manifestNamespace` as a small private helper returning the build's owning `RootNamespace` from `info.intended_ref`. `computePayloadDigest` is emitted by Phase 1a's `CasManifestCodec` — consume it verbatim, do not redefine. `LIMIT_EXCEEDED` is the existing cap error code used by `mutateShard`'s size guard.)

- [ ] **Step 4: Write the staging gtest.** In `gtest_cas_build.cpp`:

```cpp
TEST(CasBuild, StageManifestWritesBodyAtManifestKey)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStore(backend);
    auto build = store->startBuild(BuildInfo{.intended_ref = "srv1/uuid@cas@/all_1_1_0"});

    std::vector<ManifestEntry> entries;
    entries.push_back(ManifestEntry{"columns.txt", EntryPlacement::Inline, {}, 0, "x"});
    const ManifestId id = build->stageManifest(entries);

    /// The body is present at manifestKey and round-trips through the Phase-1a codec.
    const String key = store->layout().manifestKey(id);
    const auto got = backend->get(key);
    ASSERT_TRUE(got.has_value());
    const PartManifest body = decodePartManifest(got->bytes);
    EXPECT_TRUE(refMatchesBody(id.ref, body));
    EXPECT_TRUE(manifestNamespaceMatches(id.root_namespace, body));
    /// NoManifestIdReuse: a second stage of identical entries mints a DIFFERENT instance id.
    const ManifestId id2 = build->stageManifest(entries);
    EXPECT_NE(id.ref.manifest_instance_id, id2.ref.manifest_instance_id);
}

TEST(CasBuild, StageManifestCapBreachThrowsBeforeWrite)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStore(backend);
    auto build = store->startBuild(BuildInfo{.intended_ref = "srv1/uuid@cas@/all_1_1_0"});

    std::vector<ManifestEntry> entries;
    entries.push_back(ManifestEntry{"big", EntryPlacement::Inline, {}, 0, String((1ULL << 20) + 1, 'a')});
    expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED, [&] { build->stageManifest(entries); });
    /// Fail-closed: nothing was written to the pool (no _manifests object for this build).
    const auto page = backend->list(store->poolConfig().pool_prefix + "/roots/", "", 1000);
    for (const auto & k : page.keys)
        EXPECT_EQ(k.key.find("/_manifests/"), String::npos);
}
```

- [ ] **Step 5: Build the test binary's `CasBuild` TU** (downstream `precommit`/`publish` removal lands in T3–T5; if `CasBuild.cpp` does not yet compile because `precommit`/`publish` still reference removed types, gate this step to compile-only of the staging path or fold it into T5's full build). Prefer: leave `precommit`/`publish` intact in this task and only ADD `stageManifest`; remove the dead tree methods in T5 once `precommitAdd`/`promote` replace their callers. Run:

```bash
ninja -C build CasBuild.cpp.o > build/build_phase1b_t2.log 2>&1
```

Expected: compiles.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_build.cpp
git commit -m "CA GC phase1b: Build::stageManifest mints ManifestId + stream-writes body + OQ7 caps fail-closed"
```

---

### Task 3: `Build::precommitAdd` in the target root shard (no `_precommits` namespace) {#task-3-build-precommitadd-in-the-target-root-shard-no-precommits-namespace}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp`
- Modify: `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces produced:** `void Build::precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id)`. (`precommitNs`/`buildRef`/`buildShard` removed.)

**Upholds:** `SingleManifestOwner` (the manifest's only owner is `precommit(build_id)` in the target shard); `PrecommitMayReferenceMissingManifest` (no body `HEAD` is a safety input — the CAS only writes the intent record).

- [ ] **Step 1: Declare `precommitAdd`; remove the precommit-namespace helpers.** In `CasBuild.h`, replace the `precommit(const TreeId &)`, `precommitNs`, `buildRef`, `buildShard` declarations with:

```cpp
    /// Build-intent owner add, written to the SAME root shard as the future committed ref (spec §Precommit
    /// Add) — there is no `_precommits` namespace. ONE root-shard CAS appending a PrecommitTransition
    /// {old=none, new=id.ref}; shard = store->shardOf(final_ref_name), so PromotePrecommit later is an
    /// atomic owner move in this same shard. Needs NO body-exists HEAD as a safety authority: GC and
    /// promotion handle a missing precommit manifest body by failing closed.
    void precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id);
```
Also rename the `bool precommitted` member to track precommit-by-`(target_ns, final_ref_name, build_id)` so `promote` knows the shard to move in (store the `target_ns`/`final_ref_name`/`ManifestRef` on the `Build`).

- [ ] **Step 2: Implement `precommitAdd`.** Replace `Build::precommit`/`precommitNs`/`buildRef`/`buildShard` in `CasBuild.cpp` with:

```cpp
void Build::precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id)
{
    requireAlive();

    /// ManifestNamespaceMatches at the source: the precommit's manifest must belong to the target
    /// namespace (its key is built from id.root_namespace). A cross-namespace precommit is a bug.
    if (id.root_namespace != target_ns.string())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "precommitAdd: manifest namespace '{}' != target namespace '{}'", id.root_namespace, target_ns.string());

    /// W-REGISTER: the target namespace must be in `gc/registry` before its first transition exists.
    store->ensureRegistered(target_ns);

    /// ONE CAS on the TARGET shard (shardOf(final_ref_name)): append PrecommitTransition{old=none,
    /// new=id.ref}. No body HEAD — a missing body is a legal fail-closed intent (spec §Precommit Add).
    store->mutateShard(target_ns, store->shardOf(final_ref_name), [&](RootShard & root)
    {
        root.precommits.push_back(PrecommitTransition{
            .transition_version = root.shard_version + 1,
            .build_id = build_id,
            .final_ref_name = final_ref_name,
            .old_manifest = std::nullopt,
            .new_manifest = id.ref});
    });

    precommit_target_ns = target_ns;
    precommit_final_ref = final_ref_name;
    precommit_manifest = id.ref;
    precommitted = true;

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::Precommit;
        e.namespace_ = target_ns.string();
        e.ref_name = final_ref_name;
        e.token = u128ToHex(build_id);
        e.round = store->retireView().round();
        e.outcome = "ok";
        e.reason = "precommitAdd: build-intent owner add in the target shard (owner = precommit(build_id))";
        e.detail = {{"build_seq", std::to_string(build_seq)}};
    });
}
```

- [ ] **Step 3: Write the precommit gtest.** In `gtest_cas_build.cpp`:

```cpp
TEST(CasBuild, PrecommitAddWritesTransitionInTargetShard)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 4});
    const RootNamespace ns{"srv1/uuid@cas@"};
    auto build = store->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_1_1_0"});

    const ManifestId id = build->stageManifest({ManifestEntry{"columns.txt", EntryPlacement::Inline, {}, 0, "x"}});
    build->precommitAdd(ns, "all_1_1_0", id);

    /// The PrecommitTransition is in the TARGET shard (shardOf(final_ref_name)) — NOT a `_precommits` ns.
    const uint64_t shard = shardOfForTest("all_1_1_0", 4);
    const auto got = backend->get(store->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(got.has_value());
    const RootShard root = decodeRootShard(got->bytes);
    ASSERT_EQ(root.precommits.size(), 1u);
    EXPECT_EQ(root.precommits.at(0).final_ref_name, "all_1_1_0");
    ASSERT_TRUE(root.precommits.at(0).new_manifest.has_value());
    EXPECT_EQ(root.precommits.at(0).new_manifest->manifest_instance_id, id.ref.manifest_instance_id);
    /// No `_precommits` segment anywhere in the pool.
    const auto page = backend->list(store->poolConfig().pool_prefix + "/roots/", "", 1000);
    for (const auto & k : page.keys)
        EXPECT_EQ(k.key.find("/_precommits"), String::npos);
}
```

- [ ] **Step 4: Commit** (build deferred to T5; downstream `publish` still present)

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_build.cpp
git commit -m "CA GC phase1b: Build::precommitAdd records PrecommitTransition in the target root shard"
```

---

### Task 4: Blob uploads under precommit (spec order: body → precommit → blobs → promote) {#task-4-blob-uploads-under-precommit-spec-order-body-precommit-blobs-promote}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (comments/ordering only — `putBlob`/`uploadFromSource` are reused unchanged)
- Modify: `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces produced:** none new — this task asserts the modeled ordering (spec §Blob Uploads Under Precommit) and that a precommit may reference a not-yet-uploaded blob.

**Upholds:** `PrecommitMayReferenceMissingBlob` (blobs upload *after* `precommitAdd`; a precommit edge protects future uploads of the same hashes); the rule that a precommitless speculative upload is unprotected debris.

- [ ] **Step 1: Document the order at the call boundary.** Add a comment block above `putBlob` in `CasBuild.cpp` stating the modeled order is `stageManifest` (body) → `precommitAdd` → `putBlob` (blob bodies) → `promote`, and that a blob uploaded before `precommitAdd` is unprotected speculative debris GC may delete (the writer re-uploads from source or aborts at promotion). No logic change — `putBlob` (`.cpp:99`) and `uploadFromSource` (`.cpp:277`) already implement INV-1.

- [ ] **Step 2: Write the ordering gtest.** In `gtest_cas_build.cpp`:

```cpp
TEST(CasBuild, PrecommitMayReferenceNotYetUploadedBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    const RootNamespace ns{"srv1/uuid@cas@"};
    auto build = store->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_1_1_0"});

    /// Manifest references a blob hash whose body is NOT uploaded yet (spec order: body→precommit→blobs).
    const String payload = "blob-bytes";
    const BlobId blob = idOf(payload);
    std::vector<ManifestEntry> entries;
    entries.push_back(ManifestEntry{"data.bin", EntryPlacement::Blob, u128Of(payload), payload.size(), ""});
    const ManifestId id = build->stageManifest(entries);

    /// precommitAdd succeeds with the blob body still absent — NOT corruption (PrecommitMayReferenceMissingBlob).
    build->precommitAdd(ns, "all_1_1_0", id);
    EXPECT_FALSE(backend->head(store->layout().blobKey(blob)).exists);

    /// Now the blob uploads under the live precommit edge.
    build->putBlob(blob, BlobSource::fromString(payload));
    EXPECT_TRUE(backend->head(store->layout().blobKey(blob)).exists);
}
```

- [ ] **Step 3: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_build.cpp
git commit -m "CA GC phase1b: assert blob-under-precommit order (body->precommit->blobs->promote)"
```

---

### Task 5: Atomic `Build::promote` — single-shard CAS owner move + fail-closed revalidation {#task-5-atomic-build-promote-single-shard-cas-owner-move-fail-closed-revalidation}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp`
- Modify: `src/Disks/tests/gtest_cas_build.cpp`, `src/Disks/tests/gtest_cas_build_root_dangle.cpp`

**Interfaces produced:** `void Build::promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 build_id, const ManifestId & id)`. Removes the now-dead `publish`/`checkAndResolveDeps`/`recreateTree`/`adoptTree`/`adoptFromTree`/`stageTree` machinery that the tree model required. (Keep `putBlob`/`uploadFromSource`/`observeAndAdmit`/the dep set — blob revalidation still uses them.)

**Upholds:** `CommittedManifestBodyRequired` / `NoCommittedDangle` (revalidate body + every blob leaf before the owner move; missing body/blob/condemned ⇒ `ABORTED`); `SingleManifestOwner` (one CAS replaces `precommit(build_id)` with `ref(final_ref_name)` — never a window with neither owner); `ManifestActivationMatchesEdges` (missing-body activation ⇒ promotion is folded as a committed add — emit `+` edges; already-activated ⇒ pure move, no blob delta); `RefMatchesBody`/`ManifestNamespaceMatches`.

- [ ] **Step 1: Declare `promote`; delete the tree-model methods.** In `CasBuild.h`, add the `promote` declaration; delete the declarations for `publish`, `stageTree`, `uploadStagedTree`, `putTree`, `adoptTree`, `adoptFromTree`, `recreateTree`, `buildStagedClosure`, and `precommit`. Keep `putBlob`, `uploadFromSource`, `observeAndAdmit`, `recordPendingBlobDep`, `adoptEvidence`, `depIsTokened`, `hasDep`, `checkAndResolveDeps` (blob-only — the loop already keys deps by `(kind, hash)`; trees no longer appear).

```cpp
    /// Atomic commit promotion (spec §Promote Precommit): ONE root-shard CAS in shardOf(final_ref_name).
    ///  1. (mutateShard refreshes the retire view if the shard/registry fence demands it)
    ///  2. stream-read the precommit manifest body; validate RefMatchesBody / ManifestNamespaceMatches;
    ///  3. revalidate EVERY blob leaf listed in the manifest (fail-closed);
    ///  4. body absent | a blob absent | a blob condemned-and-not-recreatable ⇒ ABORTED;
    ///  5. atomically replace precommit(build_id) owner with ref(final_ref_name) owner (erase the
    ///     PrecommitTransition's live state, set refs[final_ref_name], append the journal records);
    ///  6. append PromotePrecommit (already-activated ⇒ no blob delta) OR, if the precommit was
    ///     missing-body, fold it as a committed add (OwnerTransition old=none,new=id.ref → emits + edges).
    void promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 build_id, const ManifestId & id);
```

- [ ] **Step 2: Implement `promote`.** In `CasBuild.cpp` (replacing `publish`), revalidate inside the `mutateShard` lambda so a fence-advanced conflict re-runs the gate on the fresh fence round (the same property `publish` relied on):

```cpp
void Build::promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 promote_build_id, const ManifestId & id)
{
    requireAlive();

    if (id.root_namespace != target_ns.string())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "promote: manifest namespace '{}' != target namespace '{}'", id.root_namespace, target_ns.string());

    const uint64_t registry_fence = store->ensureRegistered(target_ns);

    /// Read + validate the manifest body ONCE (O(manifest entries), one streaming read). Absent or
    /// invalid ⇒ fail closed: a committed ref must never name a missing/mismatched manifest.
    const String manifest_key = store->layout().manifestKey(id);
    const auto body_got = store->backend().get(manifest_key);
    if (!body_got)
        throw Exception(ErrorCodes::ABORTED,
            "promote: manifest body absent at {} — failing closed (retry with a fresh ManifestId)", manifest_key);
    const PartManifest body = decodePartManifest(body_got->bytes);
    if (!refMatchesBody(id.ref, body))
        throw Exception(ErrorCodes::ABORTED, "promote: RefMatchesBody failed for {}", manifest_key);
    if (!manifestNamespaceMatches(target_ns.string(), body))
        throw Exception(ErrorCodes::ABORTED, "promote: ManifestNamespaceMatches failed for {}", manifest_key);

    store->mutateShard(target_ns, store->shardOf(final_ref_name), [&](RootShard & root)
    {
        /// Refresh-then-revalidate (W-REGISTER ordering, load-bearing): if the shard's fence_round
        /// (floored by the registry fence) is ahead of our view, GC advanced — refresh first.
        if (store->retireView().round() < std::max(root.fence_round, registry_fence))
            store->retireView().refresh();

        /// Fail-closed blob revalidation of EVERY blob leaf (spec §Promote Precommit step 3). A condemned
        /// blob is recreatable only from this build's own source (INV-1); putBlob/uploadFromSource already
        /// re-upload from held bytes. A missing or non-recreatable condemned blob ⇒ ABORTED.
        for (const ManifestEntry & e : body.entries)
        {
            if (e.placement != EntryPlacement::Blob)
                continue;
            const BlobId blob_id{u128ToHex(e.blob_hash)};
            const String blob_key = store->layout().blobKey(blob_id);
            const HeadResult hr = store->backend().head(blob_key);
            if (!hr.exists)
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} absent at commit revalidation — failing closed", blob_key);
            if (store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token))
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
        }

        /// Was this precommit folded as missing-body (never contributed blob edges)? We cannot observe GC
        /// fold state from the writer, so the SAFE choice is: the promotion record always names the
        /// manifest; GC decides (spec §Fold Owner Transitions) whether to emit committed + edges (if the
        /// precommit was missing-body / not-yet-active) or no delta (already active). We therefore append
        /// BOTH the precommit close (old=ref,new=none) and the committed add (PromotePrecommit), atomically.
        const uint64_t v = root.shard_version + 1;
        root.precommits.push_back(PrecommitTransition{
            .transition_version = v, .build_id = promote_build_id, .final_ref_name = final_ref_name,
            .old_manifest = id.ref, .new_manifest = std::nullopt});
        root.refs[final_ref_name] = RootRef{
            .ref_name = final_ref_name, .manifest_ref = id.ref,
            .mutable_files = pending_mutable_files, .published_at_ms = nowMs()};
        root.promotions.push_back(PromotePrecommit{
            .transition_version = v, .build_id = promote_build_id,
            .final_ref_name = final_ref_name, .manifest = id.ref});
    });

    precommitted = false;
    store->retireBuildSeq(build_seq);
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BuildPublish;
        e.namespace_ = target_ns.string();
        e.ref_name = final_ref_name;
        e.object_hash = u128ToHex(id.ref.manifest_instance_id);
        e.token = u128ToHex(promote_build_id);
        e.outcome = "promoted";
        e.reason = "promote: atomic owner move precommit(build_id) -> ref(final_ref_name) after fail-closed reval";
        e.detail = {{"build_seq", std::to_string(build_seq)}};
    });
}
```
(Where the journal carries `PromotePrecommit`: the canonical `RootShard` in [Canonical Contract](#canonical-contract) already carries `std::vector<PromotePrecommit> promotions` as a first-class third record vector — T1 defines, encodes, and round-trips it. Append the `PromotePrecommit` here into `root.promotions`; replace the `PromotePrecommitToTransition` placeholder with a direct `root.promotions.push_back(...)`. Confirm against the Phase 0 model's `WPromote` so the GC fold in Phase 1d reads exactly this record.)

- [ ] **Step 3: Write the happy-promote gtest.** In `gtest_cas_build.cpp`:

```cpp
TEST(CasBuild, PromoteMovesOwnerAtomically)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    const RootNamespace ns{"srv1/uuid@cas@"};
    auto build = store->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_1_1_0"});

    const String payload = "abc";
    const BlobId blob = idOf(payload);
    const ManifestId id = build->stageManifest(
        {ManifestEntry{"data.bin", EntryPlacement::Blob, u128Of(payload), payload.size(), ""}});
    build->precommitAdd(ns, "all_1_1_0", id);
    build->putBlob(blob, BlobSource::fromString(payload));
    build->promote(ns, "all_1_1_0", build->buildId(), id);

    /// resolveRef now resolves the committed ref to the SAME ManifestRef (Phase 1c reads it; here we
    /// assert the root-journal state directly).
    const auto got = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(got.has_value());
    const RootShard root = decodeRootShard(got->bytes);
    ASSERT_TRUE(root.refs.contains("all_1_1_0"));
    EXPECT_EQ(root.refs.at("all_1_1_0").manifest_ref.manifest_instance_id, id.ref.manifest_instance_id);
    ASSERT_EQ(root.promotions.size(), 1u);
    EXPECT_EQ(root.promotions.at(0).final_ref_name, "all_1_1_0");
}
```

- [ ] **Step 4: Write the three fail-closed-branch gtests.** In `gtest_cas_build_root_dangle.cpp` (the dangle-focused suite):

```cpp
TEST(CasBuildRootDangle, PromoteFailsClosedOnMissingBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    const RootNamespace ns{"srv1/uuid@cas@"};
    auto build = store->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_1_1_0"});
    const ManifestId id = build->stageManifest({ManifestEntry{"c.txt", EntryPlacement::Inline, {}, 0, "x"}});
    build->precommitAdd(ns, "all_1_1_0", id);
    /// Delete the body out-of-band (orphan-sweep race / GC) then promote ⇒ ABORTED, no committed ref.
    const auto hr = backend->head(store->layout().manifestKey(id));
    backend->deleteExact(store->layout().manifestKey(id), hr.token);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "all_1_1_0", build->buildId(), id); });
    const auto got = backend->get(store->layout().rootShardKey(ns, 0));
    if (got) EXPECT_FALSE(decodeRootShard(got->bytes).refs.contains("all_1_1_0"));
}

TEST(CasBuildRootDangle, PromoteFailsClosedOnMissingBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    const RootNamespace ns{"srv1/uuid@cas@"};
    auto build = store->startBuild(BuildInfo{.intended_ref = ns.string() + "/all_1_1_0"});
    const String payload = "never-uploaded";
    const ManifestId id = build->stageManifest(
        {ManifestEntry{"d.bin", EntryPlacement::Blob, u128Of(payload), payload.size(), ""}});
    build->precommitAdd(ns, "all_1_1_0", id);
    /// Blob body never uploaded ⇒ committed revalidation fails closed.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "all_1_1_0", build->buildId(), id); });
}

TEST(CasBuildRootDangle, PromoteFailsClosedOnCondemnedBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// inject a condemned token for the blob hash before opening the Store (helpers::injectRetire);
    /// stage+precommit+upload, condemn the live token via injectRetire on reopen, then promote ⇒ ABORTED.
    /// (Mirror the existing condemned-token fixtures in gtest_cas_build.cpp.)
}
```

- [ ] **Step 5: Build the full CA test binary** (now `precommit`/`publish`/tree methods are gone and all callers in tests use the new API). Run:

```bash
ninja -C build unit_tests_dbms > build/build_phase1b_t5.log 2>&1
```

Expected: clean compile. (If `ContentAddressedTransaction.cpp` still calls `publish`/`precommit`, it will fail — that is T6; if it lands earlier in the build graph, do T6's edits in the same pass before this build. Prefer ordering: implement T6 edits to `ContentAddressedTransaction.cpp` immediately after T5 Step 2 if the link fails, then build once.)

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp \
        src/Disks/tests/gtest_cas_build.cpp src/Disks/tests/gtest_cas_build_root_dangle.cpp
git commit -m "CA GC phase1b: atomic Build::promote (single-shard owner move + fail-closed reval)"
```

---

### Task 6: `republishRef` → fresh destination `PartManifest`; both call sites; `RENAME` stays a no-op {#task-6-republishref-fresh-destination-partmanifest-both-call-sites-rename-stays-a-no-op}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Modify: `src/Disks/tests/gtest_ca_transaction.cpp`

**Interfaces produced:** rewritten `ContentAddressedTransaction::republishRef` (`.cpp:134`); unchanged call sites `:871` (RENAME loop over `listRefs`) and `:1025` (part-dir move). `Atomic` database `RENAME TABLE` remains a CA no-op (UUID-keyed root namespace).

**Upholds:** `SingleManifestOwner` (manifests are never shared/moved across refs or namespaces — the destination gets a fresh `ManifestId` over the same blob hashes; blob dedup gives the storage win); `NoManifestIdReuse`.

- [ ] **Step 1: Rewrite `republishRef`.** Replace the body (`.cpp:134-165`) with: resolve the source ref to its `ManifestId` + mutable payload (`Store::resolveRef`), read the source body via the Phase 1c read-path surface (`Store::readManifest(src_id).entries` — the same fail-closed reader the query path uses; do NOT add a separate `readManifestEntries` helper), `startBuild` for the destination, `stageManifest` a FRESH manifest carrying the same `ManifestEntry`s (same blob hashes — blobs are shared, metadata is duplicated), re-`putBlob` is NOT needed (the blobs already exist; the entries reference the same hashes), `precommitAdd(dst_ns, dst_ref, fresh_id)`, `promote(dst_ns, dst_ref, build_id, fresh_id)`, then `dropRef(src_ns, src_ref)`:

```cpp
bool ContentAddressedTransaction::republishRef(
    const Cas::RootNamespace & src_ns, const std::string & src_ref,
    const Cas::RootNamespace & dst_ns, const std::string & dst_ref)
{
    /// Manifests are single-owner and never shared/moved across refs or namespaces (spec §Part Manifest
    /// Ownership). A move publishes a FRESH destination part manifest over the SAME blob hashes (blob
    /// dedup gives the storage win), then drops the source ref. Returns false (nothing written) when the
    /// source ref is absent.
    auto resolved = metadata_storage.store()->resolveRef(src_ns, src_ref);
    if (!resolved)
        return false;

    /// The destination's entries are the source manifest's entries (same blob hashes, same inline
    /// payloads). Read the source body via the Phase 1c read-path surface — readManifest fails closed on
    /// a missing/mismatched committed body, so the move never silently fabricates a destination.
    std::vector<Cas::ManifestEntry> entries =
        metadata_storage.store()->readManifest(resolved->manifest_id).entries;

    auto build = metadata_storage.store()->startBuild(
        Cas::BuildInfo{.intended_ref = dst_ns.string() + "/" + dst_ref, .op = Cas::ProvenanceOp::Other});
    const Cas::ManifestId fresh = build->stageManifest(std::move(entries));   /// fresh ManifestId, same blobs
    build->setPendingMutableFiles(resolved->mutable_files);                   /// carry mutable payload to dst
    build->precommitAdd(dst_ns, dst_ref, fresh);
    build->promote(dst_ns, dst_ref, build->buildId(), fresh);                 /// fail-closed reval over shared blobs
    metadata_storage.store()->dropRef(src_ns, src_ref);
    return true;
}
```
(Consume Phase 1c's `Store::readManifest(const ManifestId &)` and `Resolved.manifest_id` verbatim — do NOT add a separate `readManifestEntries` accessor. `Build::setPendingMutableFiles` sets the `pending_mutable_files` member `promote` writes into `RootRef.mutable_files`.)

- [ ] **Step 2: Confirm the two call sites are unchanged in shape.** `:871` (`republishRef(from_ns, ref, to_ns, ref)` in the RENAME loop) and `:1025` (`republishRef(src->ns, src->ref, dst->ns, dst->ref)` in `moveDirectory`) keep the same signature — only the implementation changed. No edit needed beyond confirming they compile. The `Atomic` `RENAME TABLE` path stays a CA no-op because the root namespace is UUID-keyed (the storage root does not move); add/keep the comment that says so.

- [ ] **Step 3: Write the republish gtest.** In `gtest_ca_transaction.cpp`:

```cpp
TEST(CaTransaction, RepublishRefCreatesFreshManifestOverSameBlobs)
{
    /// Build + promote a source ref with one blob, then republish src -> dst.
    /// Assert: dst resolves to a DIFFERENT ManifestRef than src (fresh ManifestId); the blob object is
    /// the SAME key (shared, not re-uploaded); src ref is gone after the move.
    // ... open store, build src, promote, capture src ManifestRef ...
    // republishRef(src_ns, "src", dst_ns, "dst");
    // EXPECT_NE(dst.manifest_ref.manifest_instance_id, src_manifest_ref.manifest_instance_id);
    // EXPECT_TRUE(backend->head(blobKey).exists);   // same blob key, one object
    // EXPECT_FALSE(resolveRef(src_ns, "src"));
}

TEST(CaTransaction, AtomicRenameIsCaNoOp)
{
    /// An Atomic-database RENAME TABLE does not move the CA root (UUID-keyed): the ManifestId and the
    /// root namespace are unchanged. Assert resolveRef(ns, ref) returns the SAME ManifestRef before and
    /// after the rename path (no manifest operation occurred).
}
```

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "CA GC phase1b: republishRef publishes a fresh dst manifest over shared blobs; RENAME stays a CA no-op"
```

---

### Task 7: Writer best-effort `_manifests` debris cleanup on `abandon` {#task-7-writer-best-effort-manifests-debris-cleanup-on-abandon}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp`
- Modify: `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces produced:** `Build` records the `ManifestId`s it staged; `abandon` best-effort exact-token-deletes its own `_manifests/<writer_instance_id>/<build_sequence>/` objects.

**Upholds:** `OrphanManifestDebrisDrains` (the common case is writer cleanup; the GC backstop sweep — Phase 1d — handles stopped writers). Best-effort: a failed cleanup is benign (Phase-1d sweep reclaims it); never throw from `abandon`.

- [ ] **Step 1: Record staged ids.** In `CasBuild.h` add `std::vector<ManifestId> staged_manifests;` and push to it at the end of `stageManifest` (T2). In `abandon`, after `retireBuildSeq`, best-effort delete each staged manifest body with an exact token (HEAD then `deleteExact`), swallowing per-object errors:

- [ ] **Step 2: Implement the cleanup in `abandon`.** Extend `Build::abandon` (`.cpp:1081`):

```cpp
void Build::abandon()
{
    requireAlive();
    store->retireBuildSeq(build_seq);
    alive = false;

    /// Best-effort writer cleanup of THIS build's pre-precommit/staged `_manifests` debris (spec
    /// §Pre-Precommit Part-Manifest Debris). The common case is writer cleanup; a missed object is
    /// benign — the Phase-1d namespace-scoped orphan sweep reclaims it. Exact-token delete only; never
    /// throw from abandon.
    for (const ManifestId & id : staged_manifests)
    {
        try
        {
            const String key = store->layout().manifestKey(id);
            const HeadResult hr = store->backend().head(key);
            if (hr.exists)
                store->backend().deleteExact(key, hr.token);
        }
        catch (...) {}   /// best-effort: GC backstop sweep is the durable guarantee
    }

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BuildAbort;
        e.token = u128ToHex(build_id);
        e.outcome = "abandoned";
        e.reason = "abandon: best-effort _manifests debris cleanup; remainder reaped by the orphan sweep";
        e.detail = {{"build_seq", std::to_string(build_seq)}, {"staged", std::to_string(staged_manifests.size())}};
    });
}
```

- [ ] **Step 3: Write the cleanup gtest.** In `gtest_cas_build.cpp`:

```cpp
TEST(CasBuild, AbandonCleansItsOwnStagedManifestDebris)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
    auto build = store->startBuild(BuildInfo{.intended_ref = "srv1/uuid@cas@/all_1_1_0"});
    const ManifestId id = build->stageManifest({ManifestEntry{"c.txt", EntryPlacement::Inline, {}, 0, "x"}});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(id)).exists);
    build->abandon();
    /// Best-effort cleanup removed the staged body.
    EXPECT_FALSE(backend->head(store->layout().manifestKey(id)).exists);
}
```

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_build.cpp
git commit -m "CA GC phase1b: writer best-effort _manifests debris cleanup on abandon"
```

---

### Task 8: Build + full `Cas*`/`Ca*` gtest sweep {#task-8-build-full-cas-ca-gtest-sweep}

**Files:** none (verification + commit only)

- [ ] **Step 1: Build the test binary** (redirect; analyze the log with a subagent — return only a concise summary):

```bash
ninja -C build unit_tests_dbms > build/build_phase1b_t8.log 2>&1
```

Expected: clean build, no warnings-as-errors. (If anything still references `RefPayload`/`JournalRecord`/`ClosureNode`/`stageTree`/`precommit`/`publish`, fix the caller to the new API — the removal is total; CA is pre-release, no compat path.)

- [ ] **Step 2: Run the full CA gtest sweep** (redirect to a unique log; analyze with a subagent):

```bash
./build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_phase1b_sweep.log 2>&1
```

Expected: `[  PASSED  ]` with **0 failures**; the new `CasBuild`/`CasBuildRootDangle`/`CasRootShardCodec`/`CaTransaction` tests all green.

- [ ] **Step 3: Verify the phase exit** — confirm: (a) the build is clean; (b) the full `Cas*`/`Ca*` sweep passes; (c) no `RefPayload`/`JournalRecord`/`ClosureNode`/`_precommits`/`stageTree`/`publish` symbol remains (`grep -rn` over `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` returns nothing in non-history code). Phase 1b is then complete; Phase 1d's behavior switch + soak run after 1a+1b+1c are all landed.

- [ ] **Step 4: Commit**

```bash
git commit --allow-empty -m "CA GC phase1b: full Cas*/Ca* gtest sweep green — write path on root-local manifests"
```

---

## Self-Review {#self-review}

- **Spec coverage:** every sub-section of spec §Build And Precommit Protocol is a task — Stage Part Manifest (T2), Pre-Precommit Part-Manifest Debris / writer side (T7), Final Ref Name Requirement (T3, `final_ref_name` up front so the precommit shares `shardOf(final_ref_name)`), Precommit Add (T3), Blob Uploads Under Precommit (T4), Promote Precommit (T5), Write Path Budget (T5 honours one streaming manifest read + ≤U blob `HEAD`s + one `casPut`), Abandon Or Reclaim Precommit (T5 promote close + T7 abandon). §Root Journal Format → T1. §Part Manifest Ownership (no sharing/moving; `Atomic` rename no-op) → T6. ✓
- **Canonical contract:** Phase 1a's `ManifestRef`/`ManifestId`/`ManifestEntry`/`PartManifest`/`encodePartManifest`/`decodePartManifest`/`refMatchesBody`/`manifestNamespaceMatches`/`CasLayout::manifestKey` are consumed, not redefined; Phase 1b emits exactly `RootRef`/`OwnerTransition`/`PrecommitTransition`/`PromotePrecommit`/`RootShard` (T1) and `stageManifest`/`precommitAdd`/`promote` (T2/T3/T5), with `buildStagedClosure`/`ClosureNode`/`JournalRecord`/`RefPayload` removed. ✓
- **Contract is the single source of truth:** the canonical `RootShard` lists all three record vectors `{transitions, precommits, promotions}` up-front; `PromotePrecommit` is a first-class GC-foldable record (not an afterthought). T1 defines/encodes/round-trips it, T5 appends to `root.promotions`, and the promote impl confirms against the Phase 0 `WPromote` action. **Spec contract to honor in Phase 1d:** the fold reads `promotions` as committed-add-or-pure-move per `ManifestActivationMatchesEdges`. ✓
- **Invariant map:** the [task map](#spec-invariants-this-phase-upholds-task-map) ties each task to `SingleManifestOwner`, `CommittedManifestBodyRequired`, `PrecommitMayReferenceMissingManifest`/`Blob`, `ManifestActivationMatchesEdges`, `MutablePayloadNotReachability` (a mutable-only update = a root-shard CAS changing `RootRef.mutable_files`, emitting no owner transition / no blob delta / no id change), `NoManifestIdReuse`, `OrphanManifestDebrisDrains`. ✓
- **TDD + no placeholders:** every code step shows actual code; every run step shows the exact command + expected output; each task ends with a commit. Cross-phase surfaces are consumed verbatim, not reinvented: `stageManifest` sets `payload_digest` via Phase 1a's `computePayloadDigest`, and `republishRef` reads source entries via Phase 1c's `Store::readManifest(id).entries` (no bespoke `readManifestEntries` helper). ✓
- **Style:** Allman braces throughout; only contract type names used; `f` not `f`-applied in prose; "exception" not "crash". ✓

**Gate reminder:** this phase's code tasks presuppose Phase 0 GREEN and Phase 1a landed. Phase 1d (the GC fold + behavior switch) consumes the `RootShard.transitions`/`precommits`/`promotions` records this phase writes.
