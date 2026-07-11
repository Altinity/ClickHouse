# CAS mixed-algo pools (Phase 3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax. Every task below is SELF-CONTAINED: it carries the exact types, signatures, test code and commands — implementers do not need to read the design spec.

**Goal:** a CAS pool may hold blobs under several hash algos simultaneously (additive switching, no migration); the blob identity is the pair `BlobRef{algo, digest}` everywhere; a bare digest ceases to exist as a blob identity; admitting a new algo is explicit opt-in.

**Architecture:** new `BlobRef` pair type → manifests carry per-entry `BlobRef` (write path first) → GC settlement moves to algo-prefixed key schema 3 and every settlement structure retypes to `BlobRef` (schemas 1/2 and digest-first helpers are DELETED) → `PoolMeta` gets an append-only `algos_used` set with flag-gated admission → sweep/fsck derive the algo from the blob-key path; the manifest-read boundary validates admission with refresh-on-miss.

**Tech stack:** C++ (ClickHouse dbms), gtest (`unit_tests_dbms`), InMemoryBackend for GC tests.

**Design:** `docs/superpowers/specs/2026-07-11-cas-mixed-algo-pools-design.md` (rev.6, user-approved). Consult record in its §12.

## Global Constraints

- Branch `cas-gc-rebuild`. NEVER commit to master. New commits only — no rebase, no amend.
- Build: `cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_p3tN.log 2>&1; echo NINJA_EXIT=$?` (N = task number). NEVER dump the log into context — grep for `error:` and `NINJA_EXIT`. Run the build to completion IN YOUR OWN TURN — do NOT hand off to a background monitor.
- Tests: `./src/unit_tests_dbms --gtest_filter='<filter>'` from the build dir. The full regression gate for every task: `--gtest_filter='*Cas*:*BlobRef*:*BlobDigest*:*InDegree*:*Gc*:*Manifest*:*Fsck*:*Store*:*Build*:*Meta*:*Dedup*'` — ALL must pass (2 pre-existing DISABLED tests are expected and fine).
- Scratch files: `/home/mfilimonov/workspace/ClickHouse/master/tmp`, never `/tmp`.
- C++ style: Allman braces (opening brace on its own line). Never `sleep` to fix a race. In comments/messages say "exception", not "crash"; wrap code names in backticks.
- CA is PRE-RELEASE: no persisted-data compatibility, no legacy read paths, no version bumps for the wire changes below. Old runs/manifests are disposable derived state.
- **The prime directive (user decree):** after this plan, a blob content hash NEVER appears without its algo — no API parameter, no container key, no on-wire key, no rendered id. `BlobRef` is constructed ONLY (a) by the write mint (the hasher knows its algo) and (b) by durable-form parsers (settlement key codec, blob-path parser, manifest decoder). Everything else copies `BlobRef`s. Digest-only blob-identity overloads are DELETED, not deprecated.
- Two temporary expressions are permitted ONLY where a task explicitly names them, and the named later task deletes them; Task 6 ends with grep-gates proving zero survivors.
- Commit trailers (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```
- Report contract (every task): write the full report to `/home/mfilimonov/.claude/jobs/2dcf2af7/tmp/p3tN-report.md`; return only STATUS + commit sha + one-line test summary + concerns.

## Existing code you build on (reference — verbatim from the tree)

- `Core/CasBlobDigest.h`: `struct BlobDigest { std::array<uint8_t,32> bytes{}; <=>; ==; fromU128; toU128; }`, `BlobDigestHash` (FNV-1a), `class DigestCodec { DigestCodec(uint64_t len); toHex; fromHex; toBytesBE; fromBytesBE; shardOf; digestLen(); }` (the `DigestCodec(const PoolMeta&)` ctor is deleted in Task 4).
- `Core/CasBlobHasher.h`: `enum class BlobHashAlgo : uint8_t { CityHash128=1, XXH3_128=2, Sha256=3 }`, `blobHashAlgoName(algo)` → `"ch128"|"xxh3"|"sha256"`, `blobHashLenFor(algo)` → 16|16|32, `parseBlobHashAlgo(str)`, `makeBlobHashingWriteBuffer`, `blobHashHexOneShot`.
- `Core/CasManifestCodec.h`: `ManifestEntry { String path; EntryPlacement placement; BlobDigest blob_hash; uint64_t blob_size; String inline_bytes; ==default; }`, `PartManifest { ManifestRef ref; RootNamespace root_namespace_id; uint8_t blob_hash_len = 16; UInt128 payload_digest; std::vector<ManifestEntry> entries; }`.
- `Core/CasBlobInDegree.h`: `kSourceEdgeKeySchema128=1`, `kSourceEdgeKeySchemaSha256=2`, `sourceEdgeDigestLen`, `sourceEdgeKeySchemaFor`, `class SourceEdgeKeyCodec { SourceEdgeKeyCodec(uint8_t len); forSchema; key(BlobDigest,UInt128); parse; seekPrefix; }`, `struct BlobDelta { BlobDigest blob_hash; UInt128 source_id; bool remove; }`, `struct BlobCandidate { BlobDigest hash; }`, `foldDeltasIntoGeneration(..., uint8_t digest_len = 16)`, `inDegreeInGeneration(backend, runs, const BlobDigest&)`, sentinel rows `kZeroMarker/kCondemned` at `source_id==0`.
- `Core/CasGcShardPlan.h`: `blobShard(const BlobDigest&, gc_shards)` (BE-u64 of bytes[0:8] % shards), `ShardReducer { owns(BlobDigest); reduce(..., uint8_t digest_len = 16); }`.
- `Core/CasGcFormats.h`: `RetiredEntry { ObjectKind kind; BlobDigest hash; Token token; uint64_t size; uint64_t condemn_round; ... }`, `ReplacedEntry { RetiredEntry fresh; Token old_token; }`.
- `Core/CasGcOutcomes.h`: `OutcomeEntry { ...; BlobDigest hash; Token token; ... }`.
- `Core/CasBlobMeta.h`: `loadMeta/putMetaIfAbsent/casMeta/deleteMetaExact(backend, layout, codec, const BlobDigest & hash, ...)` (a `DigestCodec` is threaded since Phase 2 T5).
- `Core/CasStore.h`: `dedupCacheContains/Add(const BlobDigest&)`, `PoolConfig { ...; Cas::BlobHashAlgo blob_hash_algo; ... }`, `Store::poolMeta()`, `Store::layout()`.
- `Core/CasBuild.h`: `using DepKey = std::pair<uint8_t, BlobDigest>; std::map<DepKey, DepEntry> deps;` `poolContentHash(BlobHashAlgo algo, uint64_t digest_len, std::string_view payload) -> BlobDigest`.
- `Core/CasLayout.h`: `blobKey(BlobId)`, `blobMetaKey(BlobId)`; the `Layout` is constructed with the pool algo and renders the `blobs/<algo>/` segment; `objectKey(...)` definition lives in `CasBuild.cpp` (sole caller).
- `Core/CasPoolMeta.{h,cpp}`: `blob_hash_algo` (u8, protobuf field), derived `blob_hash_len`, `createOrValidate(backend, layout, root_shards, blob_header_len, BlobHashAlgo)` with `checkBlobHashAlgoMatches` fail-close.
- `Core/CasGc.cpp`: `foldManifestEdges` (emits `BlobDelta` per Blob entry; validates `body.blob_hash_len == poolMeta().blob_hash_len` — replaced in Task 5), condemn-sweep (LIST `blobs/`, parse hex via pool-width codec), `rebuildBaseline` (`edge_bearing` set, `condemn_seeded` map), `previewDeletes`, `blobKeyOf(layout, codec, hash)`.
- `Core/CasFsck.cpp`: classification sets (`present_meta_hashes`, `unref_hashes`, `present_body_hashes`, ...) keyed on `BlobDigest`, parsed via pool-width `codec.fromHex`.
- `Core/CasEnvelope.h`: `EnvelopeHeader.hash_algo` (u8) — already per-blob.
- Tests live in `src/Disks/tests/gtest_cas_*.cpp`; helpers in `src/Disks/tests/cas_test_helpers.h`; in-memory backend `DB::Cas::tests::...` / `InMemoryBackend` (see any GC gtest for construction patterns).

---

### Task 1: `BlobRef` foundation type

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h`
- Test: `src/Disks/tests/gtest_cas_blob_ref.cpp` (create via editing CMake is NOT needed — the tests glob; verify by building)

**Interfaces:**
- Produces (later tasks consume verbatim):
  - `struct BlobRef { BlobHashAlgo algo; BlobDigest digest; }` ordered/hashable
  - `BlobRefHash` for unordered containers
  - `DigestCodec codecFor(BlobHashAlgo)` — the ONE way to get a codec for an algo
  - `String blobHexOf(const BlobRef &)` — bare hex at the algo's width (for KEY construction)
  - `String blobIdOf(const BlobRef &)` — `"<algoName>:<hex>"` (for logs/events/inspect ONLY)

- [ ] **Step 1: write the header**

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>

namespace DB::Cas
{

/// THE blob identity (mixed-algo pools, design 2026-07-11 §2): the PAIR of the hash algo and the
/// digest. A bare digest is NOT a blob identity anywhere -- `ch128` and `xxh3` digests are both
/// 16-byte, so the same digest value under two algos names two DIFFERENT objects. BlobRef is
/// constructed ONLY where algo and digest are born together (the write mint / the hasher) or read
/// together (a durable form: settlement key, blob path, manifest entry, envelope). Every other
/// site COPIES BlobRefs -- never assemble one from an algo and a digest obtained separately.
struct BlobRef
{
    BlobHashAlgo algo = BlobHashAlgo::CityHash128;
    BlobDigest digest{};

    auto operator<=>(const BlobRef &) const = default;
    bool operator==(const BlobRef &) const = default;
};

/// Hasher for unordered_map/unordered_set keys (in-process only, not a content address).
struct BlobRefHash
{
    size_t operator()(const BlobRef & r) const noexcept
    {
        size_t h = BlobDigestHash{}(r.digest);
        h ^= static_cast<size_t>(r.algo) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

/// The ONE way to obtain a digest codec for an algo (replaces the deleted pool-wide
/// `DigestCodec(PoolMeta)`): width follows the algo, never a pool-level assumption.
inline DigestCodec codecFor(BlobHashAlgo algo)
{
    return DigestCodec(blobHashLenFor(algo));
}

/// Bare lowercase hex of the digest at the algo's width -- for OBJECT KEY construction only
/// (the algo lives in the key's path segment `blobs/<algo>/...`).
inline String blobHexOf(const BlobRef & r)
{
    return codecFor(r.algo).toHex(r.digest);
}

/// Human/log identity: "<algoName>:<hex>", e.g. "sha256:ab12...". Rendered ids must never be a
/// bare hex (ambiguous across algos) -- events, inspect JSON and error messages use this.
inline String blobIdOf(const BlobRef & r)
{
    return String(blobHashAlgoName(r.algo)) + ":" + blobHexOf(r);
}

}
```

- [ ] **Step 2: write the failing test**

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <unordered_set>

using namespace DB::Cas;

TEST(CasBlobRef, SameDigestDifferentAlgoAreDistinct)
{
    const BlobDigest d = BlobDigest::fromU128(UInt128(0xDEADBEEF));
    const BlobRef a{BlobHashAlgo::CityHash128, d};
    const BlobRef b{BlobHashAlgo::XXH3_128, d};
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);                                    /// algo=1 < algo=2
    std::unordered_set<BlobRef, BlobRefHash> s{a, b};
    EXPECT_EQ(s.size(), 2u);
}

TEST(CasBlobRef, OrderIsAlgoThenDigest)
{
    const BlobRef small_algo_big_digest{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(0) - 1)};
    const BlobRef big_algo_small_digest{BlobHashAlgo::Sha256, BlobDigest::fromU128(UInt128(1))};
    EXPECT_LT(small_algo_big_digest, big_algo_small_digest);   /// algo decides first
}

TEST(CasBlobRef, HexAndIdRenderAtAlgoWidth)
{
    BlobRef r16{BlobHashAlgo::XXH3_128, BlobDigest::fromU128(UInt128(0xAB))};
    EXPECT_EQ(blobHexOf(r16).size(), 32u);
    EXPECT_EQ(blobIdOf(r16).substr(0, 5), "xxh3:");
    BlobRef r32{BlobHashAlgo::Sha256, {}};
    for (size_t i = 0; i < 32; ++i) r32.digest.bytes[i] = static_cast<uint8_t>(i);
    EXPECT_EQ(blobHexOf(r32).size(), 64u);
    EXPECT_EQ(blobIdOf(r32).substr(0, 7), "sha256:");
}
```

- [ ] **Step 3: build + run** — `ninja unit_tests_dbms` (log `build_p3t1.log`), then `--gtest_filter='CasBlobRef.*'` → 3/3 PASS; then the full regression filter → all pass.
- [ ] **Step 4: commit** — message: `feat(cas): BlobRef pair type — the blob identity for mixed-algo pools (Phase 3 T1)` + trailers.

---

### Task 2: manifests carry per-entry `BlobRef`; the write path mints and copies pairs

**Files:**
- Modify: `Core/CasManifestCodec.{h,cpp}`, `Core/CasBuild.{h,cpp}`, `ContentAddressedTransaction.cpp`, `Core/CasStore.{h,cpp}`, `Core/CasLayout.h`, `Core/CasInspect.cpp`, `Core/CasFsck.cpp` (compile sites only), `Core/CasGc.cpp` (3 named TEMPORARY sites only)
- Test: `src/Disks/tests/gtest_cas_manifest_codec.cpp`, `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces:**
- Consumes: Task 1's `BlobRef`/`codecFor`/`blobHexOf`/`blobIdOf`.
- Produces:
  - `ManifestEntry { String path; EntryPlacement placement; BlobRef ref; uint64_t blob_size; String inline_bytes; }` (field `blob_hash` and `PartManifest::blob_hash_len` are DELETED)
  - entry wire: `placement u8, algo u8, digest[blobHashLenFor(algo)] raw BE, blob_size u64 LE, inline_len u32, inline` — decode throws `CORRUPTED_DATA` on an algo byte `blobHashAlgoName` rejects
  - `poolContentHash(BlobHashAlgo algo, std::string_view payload) -> BlobRef` (the ONE write mint; the old digest-returning overload is deleted)
  - `Layout::blobKey(const BlobRef &)`, `Layout::blobMetaKey(const BlobRef &)` — key = `blobs/<blobHashAlgoName(algo)>/<hex[0:2]>/<hex>`; the `Layout` no longer captures a pool algo (delete the ctor algo param; compile-drive its callers)
  - `DepKey = BlobRef` (`std::map<BlobRef, DepEntry>`), `dedupCacheContains/Add(const BlobRef &)`, `.meta` API keyed by `const BlobRef &` (codec derived inside via `codecFor(ref.algo)` — drop the threaded codec param), `PendingBlob.ref : BlobRef`, `findPendingBlob(..., const BlobRef &)`, `referenced_hashes : std::unordered_set<BlobRef, BlobRefHash>`
  - envelope: `header.hash_algo = static_cast<uint8_t>(ref.algo)` at every blob write

- [ ] **Step 1: retype + rewire (compile-driven).** Change the structs/signatures in the Interfaces block, then build repeatedly; at EVERY error apply exactly one of these patterns (no other changes):

| Error site pattern | Fix |
|---|---|
| `entry.blob_hash` (read as digest) | `entry.ref` (pass the whole pair) — if the consumer signature still wants a digest, retype the consumer to `BlobRef` (it is on the write path; all write-path consumers retype in this task) |
| `entry.blob_hash = <BlobDigest>` (mint assign) | the mint now returns `BlobRef`: `entry.ref = poolContentHash(algo, bytes);` |
| `u128ToHex(...)`/`codec.toHex(x.digest)` building a blob KEY | `blobHexOf(ref)` or `layout.blobKey(ref)` |
| a log/event `object_hash` string for a blob | `blobIdOf(ref)` |
| `DigestCodec(store->poolMeta())` / `poolMeta().blob_hash_len` | `codecFor(ref.algo)` when a `BlobRef` is in scope; when it is the WRITE mint context, `codecFor(config algo)` |
| carry-forward (`adoptEvidence`, `recordPendingBlobDep`) | copy `entry.ref` whole — this is what makes mixed manifests work |
- The algo for NEW content comes from the node's configured write algo (`PoolConfig::blob_hash_algo` — unchanged in this task). `CaContentWriteBuffer`/`stageBlobPartFile`/inline-candidate hashing: mint via `poolContentHash(write_algo, bytes)` → a `BlobRef`.
- **THREE named TEMPORARY sites** (Task 3 deletes them; do not add others): in `Core/CasGc.cpp` `foldManifestEdges`, keep the fold compiling with `entry.ref.digest` at (a) the `BlobDelta{.blob_hash = ...}` push, (b) the event `object_hash` render, and in `Core/CasFsck.cpp` (c) the manifest-entry → `BlobDigest`-set inserts. Mark each with `/// TEMPORARY(P3T3): bare digest, settlement retypes to BlobRef in Task 3`.
- `renderManifestEntry` in `CasInspect.cpp`: render `.add("blob", jsonEscape(blobIdOf(e.ref)))` (replaces the `blob_hash` + width juggling).

- [ ] **Step 2: failing tests first (add to `gtest_cas_manifest_codec.cpp`):**

```cpp
TEST(CasManifestCodec, MixedAlgoEntriesRoundTrip)
{
    PartManifest m;
    m.ref = ManifestRef{7, 1, 1};
    m.root_namespace_id = RootNamespace("srv/x@cas@");
    ManifestEntry a;                                   /// carried-forward old-algo entry
    a.path = "a.bin"; a.placement = EntryPlacement::Blob;
    a.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(0x11))};
    a.blob_size = 3;
    ManifestEntry b;                                   /// fresh new-algo entry
    b.path = "b.bin"; b.placement = EntryPlacement::Blob;
    b.ref.algo = BlobHashAlgo::Sha256;
    for (size_t i = 0; i < 32; ++i) b.ref.digest.bytes[i] = static_cast<uint8_t>(0xC0 + i);
    b.blob_size = 4;
    m.entries = {a, b};
    m.payload_digest = computePayloadDigest(m);
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    ASSERT_EQ(got.entries.size(), 2u);
    EXPECT_EQ(got.entries[0], a);
    EXPECT_EQ(got.entries[1], b);                      /// all 32 sha256 bytes survive next to a 16-byte sibling
}

TEST(CasManifestCodec, UnknownEntryAlgoFailsClosed)
{
    PartManifest m;
    m.ref = ManifestRef{7, 1, 1};
    m.root_namespace_id = RootNamespace("srv/x@cas@");
    ManifestEntry e; e.path = "a"; e.placement = EntryPlacement::Blob;
    e.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(1))};
    m.entries = {e};
    String bytes = encodePartManifest(m);
    /// entry payloads live inside the embedded RunFile; corrupt the ONE algo byte by searching for
    /// the encoded entry: placement(0x02) followed by algo(0x01) — flip algo to 99.
    const size_t pos = bytes.find(String("\x02\x01", 2));
    ASSERT_NE(pos, String::npos);
    bytes[pos + 1] = static_cast<char>(99);
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ decodePartManifest(bytes); });
}
```

- [ ] **Step 3: the W-DEP-SET cross-satisfaction crux (spec §9.9) — add to `gtest_cas_build.cpp`.** Shape (adapt helper names to the file's existing fixtures — it already builds `Store`+`Build` over an `InMemoryBackend`): stage/publish a manifest whose entries are `ch128:X` and `xxh3:X` (the SAME 16-byte digest value under two algos) where ONLY the `ch128:X` blob body physically exists in the backend. Assert publish/promote FAILS closed (the missing `xxh3:X` dependency is not satisfied by `ch128:X` evidence) and no committed ref appears. This test must be RED if `deps` were keyed on a bare digest — state that in a comment.
- [ ] **Step 4: build + full regression filter green.**
- [ ] **Step 5: commit** — `feat(cas): manifests carry per-entry BlobRef; write path mints and copies pairs (Phase 3 T2)` + trailers.

---

### Task 3: settlement key schema 3; schemas 1/2 and every digest-first form DELETED

**Files:**
- Modify: `Core/CasBlobInDegree.{h,cpp}`, `Core/CasGcShardPlan.{h,cpp}`, `Core/CasGcFormats.h`, `Core/CasGcOutcomes.h`, `Core/CasGc.{h,cpp}`, `Core/CasFsck.cpp`, `Core/CasBlobMeta.{h,cpp}` (if any digest param remains), test helpers + every gtest the compiler flags
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`, `src/Disks/tests/gtest_cas_run_file.cpp`

**Interfaces:**
- Produces:
  - `constexpr uint8_t kSourceEdgeKeySchema = 3;` (constants `...128=1`/`...Sha256=2`, `sourceEdgeDigestLen`, `sourceEdgeKeySchemaFor` and the width-stateful `SourceEdgeKeyCodec(uint8_t len)` are DELETED)
  - stateless codec (all static, no width state):
    ```cpp
    struct SourceEdgeKeyCodec
    {
        /// key = algo(u8) ++ digest[blobHashLenFor(algo)] ++ source_id(16 BE); 33 or 49 bytes.
        static String key(const BlobRef & ref, const UInt128 & source_id);
        /// throws NOT_IMPLEMENTED on an unknown algo byte, CORRUPTED_DATA on a wrong total length
        /// for a known algo. Zero-tails the digest.
        static void parse(std::string_view key, BlobRef & ref, UInt128 & source_id);
        static String seekPrefix(const BlobRef & ref);   /// algo ++ digest[len]
    };
    ```
  - `struct BlobDelta { BlobRef ref; UInt128 source_id; bool remove; }`, `struct BlobCandidate { BlobRef ref; }`, `RetiredEntry.ref : BlobRef` (field `hash` renamed), `ReplacedEntry` follows, `OutcomeEntry.ref : BlobRef`
  - `blobShard(const BlobRef & ref, uint64_t gc_shards)` — reads `ref.digest.bytes[0:8]` BE, **deliberately ignores `ref.algo`** (comment why: distribution comes from the digest; taking `BlobRef` prevents callers discarding identity)
  - `foldDeltasIntoGeneration(...)` / `ShardReducer::reduce(...)` — the `digest_len` parameter is DELETED (keys are self-describing); `head_blob`/`peek_head` callbacks: `std::function<std::optional<HeadResult>(const BlobRef &)>`
  - `inDegreeInGeneration(backend, runs, const BlobRef &)`; `blobKeyOf(layout, const BlobRef &)` = `layout.blobKey(ref)`
- Key ordering fact for the implementer: within one algo all keys share one width; keys of different algos diverge at byte 0 — raw lexicographic order == `(algo, digest, source_id)`; the merge comparator on deltas must be exactly `std::tie`-order `(ref.algo, ref.digest, source_id)` (which equals `BlobRef::operator<` then `source_id`). The duplicate-sentinel guard variable in the fold (`sentinel_blob`) becomes `BlobRef`.

- [ ] **Step 1: failing key-codec tests first (add to `gtest_cas_blob_indegree.cpp`):**

```cpp
TEST(CasSourceEdgeKeySchema3, MixedWidthKeysOrderAlgoFirst)
{
    const BlobDigest d16 = BlobDigest::fromU128((UInt128(0xFFFFFFFFFFFFFFFFULL) << 64) | 0xFFULL);
    BlobDigest d32{};                                    /// sha256 digest starting 0x00,0x01 — small bytes
    d32.bytes[1] = 0x01;
    const BlobRef ch{BlobHashAlgo::CityHash128, d16};    /// algo=1, digest all-FF prefix
    const BlobRef sh{BlobHashAlgo::Sha256, d32};         /// algo=3, tiny digest
    const String k_ch = SourceEdgeKeyCodec::key(ch, UInt128(7));   /// 33 bytes
    const String k_sh = SourceEdgeKeyCodec::key(sh, UInt128(7));   /// 49 bytes
    EXPECT_EQ(k_ch.size(), 33u);
    EXPECT_EQ(k_sh.size(), 49u);
    /// algo byte decides BEFORE any digest byte can: ch128(1) < sha256(3) even though the ch128
    /// digest bytes are all 0xFF and the sha256 digest bytes are almost all zero.
    EXPECT_LT(k_ch, k_sh);
    /// sentinel-first inside one blob group:
    EXPECT_LT(SourceEdgeKeyCodec::key(ch, UInt128(0)), k_ch);
}

TEST(CasSourceEdgeKeySchema3, ParseFailsClosed)
{
    BlobRef r; UInt128 sid;
    String k = SourceEdgeKeyCodec::key(BlobRef{BlobHashAlgo::XXH3_128, BlobDigest::fromU128(UInt128(5))}, UInt128(9));
    SourceEdgeKeyCodec::parse(k, r, sid);
    EXPECT_EQ(r.algo, BlobHashAlgo::XXH3_128);
    EXPECT_EQ(r.digest.toU128(), UInt128(5));
    EXPECT_EQ(sid, UInt128(9));
    k[0] = static_cast<char>(99);                        /// unknown algo byte
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&]{ SourceEdgeKeyCodec::parse(k, r, sid); });
    k[0] = static_cast<char>(1);                         /// known algo, wrong length (33 expected, this is 33 — truncate)
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ SourceEdgeKeyCodec::parse(std::string_view(k).substr(0, 20), r, sid); });
}

TEST(CasRunFile, MixedWidthKeysAcrossBlockBoundary)      /// spec §9.10 — add to gtest_cas_run_file.cpp
{
    /// tiny blocks: one record per block; a 33->49-byte width transition lands exactly on a block
    /// boundary; seek by both prefixes; absent prefix positions on the next greater key.
    const String k1(33, 'a'), k2(33, 'b'), k3(49, 'c'), k4(49, 'd');
    const String bytes = /* writeRun({{k1,"1"},{k2,"2"},{k3,"3"},{k4,"4"}}, 1) — reuse the file's writeRun helper */;
    RunFileReader r{std::string_view(bytes)};
    r.seek(k3.substr(0, 17));                            /// a 17-byte prefix of the 49-byte key
    String k, p;
    ASSERT_TRUE(r.next(k, p));
    EXPECT_EQ(k, k3);
}
```

- [ ] **Step 2: implement + compile sweep.** Retype per Interfaces; the fold/merge internals: comparator `(ref, source_id)`; sentinel writes via `SourceEdgeKeyCodec::key(cur_ref, kZeroSourceId)`; `PriorEdgeCursor` parses into `BlobRef` and its schema gate becomes simply `keySchema() == kSourceEdgeKeySchema (=3)`. Delete the T2 TEMPORARY sites (`foldManifestEdges` now pushes `BlobDelta{.ref = entry.ref, ...}` natively; fsck sets retype in this task or compile-minimal here + fully in Task 5). Condemn-sweep + rebuild sets (`edge_bearing`, `condemn_seeded`) become `unordered_set/map<BlobRef, ..., BlobRefHash>` — the sweep still parses the path with the POOL-width codec temporarily; mark it `/// TEMPORARY(P3T5): path-derived BlobRef lands in Task 5` (the SECOND and last permitted temporary).
- [ ] **Step 3: build + gtest filters `CasSourceEdgeKeySchema3.*:CasRunFile.*` green, then a 2-algo fold gtest:** extend an existing fold test (e.g. in `gtest_cas_gc_fold.cpp` or `gtest_cas_blob_indegree.cpp`) with deltas for `ch128:X` and `sha256:Y` in ONE shard run; assert both settle (edges present, condemn on removal works per ref) — mixed rows in one run, no algo loop.
- [ ] **Step 4: full regression filter green.**
- [ ] **Step 5: commit** — `feat(cas): settlement key schema 3 (algo-prefixed); BlobRef everywhere in GC; schemas 1/2 deleted (Phase 3 T3)` + trailers.

---

### Task 4: `PoolMeta.algos_used` + flag-gated admission; the pool-wide width dies

**Files:**
- Modify: `Core/CasPoolMeta.{h,cpp}` (+ its `.proto` if the message is generated — follow how `blob_hash_algo` is serialized today), `Core/CasStore.{h,cpp}`, `Core/CasBlobDigest.h` (delete the `DigestCodec(const PoolMeta&)` ctor), `MetadataStorageFactory.cpp`
- Test: `src/Disks/tests/gtest_cas_pluggable_hash.cpp`

**Interfaces:**
- Produces:
  - `PoolMeta.algos_used : std::vector<uint8_t>` (canonically sorted, append-only; wire: repeated field REPLACING `blob_hash_algo`; `blob_hash_len` member DELETED)
  - `PoolMeta::createOrValidate(backend, layout, root_shards, blob_header_len, BlobHashAlgo config_algo, bool allow_new)`:
    - fresh pool → create with `algos_used = {config_algo}`
    - existing pool, `config_algo ∈ algos_used` → OK
    - existing pool, not a member, `allow_new` → CAS-union loop (read+token → insert sorted → `casPut`; on conflict re-read and retry; recompute from the fresh value every retry)
    - existing pool, not a member, `!allow_new` → `BAD_ARGUMENTS`: `"CAS pool blob_hash mismatch: pool has {{{}}}; config requests {}; set <blob_hash_allow_new>1</blob_hash_allow_new> to admit a new algo into this pool"`
    - creation race: the loser re-reads; member → OK; not a member → same flag-gated union/refuse
  - `PoolConfig.blob_hash_allow_new : bool = false`; factory parses `<blob_hash_allow_new>` (default 0)
  - `Store::writeAlgo() -> BlobHashAlgo` (from config), `Store::isAlgoAdmitted(BlobHashAlgo)` — monotone in-memory set seeded from open-time `algos_used`, `Store::refreshAdmittedAlgos()` — re-reads `_pool_meta`, unions into the cache, returns the fresh set (mutex-guarded; used by Task 5's refresh-on-miss)
- Compile-drive the `DigestCodec(PoolMeta)`/`blob_hash_len` deletions: every survivor becomes `codecFor(<the BlobRef in scope>.algo)`; if a survivor has NO BlobRef in scope it is by definition a write-mint site → `codecFor(store->writeAlgo())`. There must be no other kind of survivor — if you find one, STOP and report BLOCKED with the site.

- [ ] **Step 1: failing tests first (replace/extend the Phase 2 admission tests in `gtest_cas_pluggable_hash.cpp`):**

```cpp
TEST(CasPluggableHash, AdmissionIsFlagGated)             /// spec §9.1 at the unit level
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");
    PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::CityHash128, /*allow_new*/ false);
    /// without the flag: refuse, pool untouched
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    { PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::Sha256, false); });
    /// with the flag: admitted
    const PoolMeta admitted = PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::Sha256, true);
    EXPECT_EQ(admitted.algos_used, (std::vector<uint8_t>{1, 3}));
    /// steady state: admitted algo reopens WITHOUT the flag
    const PoolMeta steady = PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::Sha256, false);
    EXPECT_EQ(steady.algos_used, (std::vector<uint8_t>{1, 3}));
}

TEST(CasPluggableHash, ConcurrentAdmissionUnions)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");
    PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::CityHash128, false);
    PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::XXH3_128, true);
    PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::Sha256, true);
    const PoolMeta final_pm = PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::CityHash128, false);
    EXPECT_EQ(final_pm.algos_used, (std::vector<uint8_t>{1, 2, 3}));   /// union, sorted, nothing lost
}
```
(Also update the Phase 2 tests that asserted the OLD single-algo fail-close semantics: the default-refuse behavior test stays — it now asserts the new message/flag hint.)

- [ ] **Step 2: implement + compile sweep per Interfaces.** `min_reader_generation`: on any successful open where the recorded value predates the schema-3 generation constant, CAS-raise it (one line where `createOrValidate` already CAS-writes; pre-release, no ceremony).
- [ ] **Step 3: build + `CasPluggableHash.*` + full regression filter green.**
- [ ] **Step 4: commit** — `feat(cas): PoolMeta.algos_used with flag-gated admission; pool-wide digest width deleted (Phase 3 T4)` + trailers.

---

### Task 5: path-derived `BlobRef` in sweep/fsck; admission validation at the fold with refresh-on-miss

**Files:**
- Modify: `Core/CasGc.cpp` (condemn-sweep, `foldManifestEdges`), `Core/CasFsck.cpp`, `Core/CasLayout.h` (path parser), `Core/CasStore.cpp` (register-before-first-write hook)
- Test: `src/Disks/tests/gtest_cas_pluggable_hash.cpp`, `src/Disks/tests/gtest_cas_fsck.cpp` (or the fsck tests' actual home — grep `runFsck` in `src/Disks/tests/`)

**Interfaces:**
- Produces:
  - `std::optional<BlobRef> Layout::parseBlobKey(std::string_view key)` — parses `<pool>/blobs/<algoName>/<h2>/<hex>` (and the `.meta` sibling): unknown/foreign algo segment or malformed hex → `std::nullopt` (callers classify as foreign debris/`unaccounted`); a KNOWN segment with wrong-width hex → `std::nullopt` too (it is not our object). This replaces the pool-width `codec.fromHex(key.substr(...))` in the condemn-sweep and all fsck listing sites — delete the Task 3 TEMPORARY.
  - `foldManifestEdges` per-entry admission check (replaces the Phase 2 width gate): `entry.ref.algo` unknown to the build → decode already threw; known but `!store->isAlgoAdmitted(algo)` → `store->refreshAdmittedAlgos()`; still absent → `CORRUPTED_DATA` `"CAS gc fold: manifest entry algo {} not admitted to this pool (algos_used {})"`. The refresh happens on EVERY miss.
  - register-before-first-write: in `Store::open`, after `createOrValidate` returns (which performed the flag-gated union), assert `isAlgoAdmitted(writeAlgo())` — belt-and-braces; the invariant "no blob/manifest/ref naming an algo is ever written before that algo is durably in `algos_used`" holds because open-time admission precedes any build.

- [ ] **Step 1: failing tests first.**

```cpp
TEST(CasPluggableHash, StaleAlgoRegistryRefreshOnMiss)   /// spec §9.8 — THE race regression
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Node B opens FIRST (its admitted-cache = {ch128}) — constructing it after A's update would miss the race.
    auto store_b = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "b", .root_shards = 1,
                                                   .blob_hash_algo = BlobHashAlgo::CityHash128});
    /// Node A admits sha256 and writes a manifest carrying a sha256 entry.
    auto store_a = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a", .root_shards = 1,
                                                   .blob_hash_algo = BlobHashAlgo::Sha256,
                                                   .blob_hash_allow_new = true});
    /// ... publish via store_a a part with one sha256 blob (reuse the file's existing publish helper) ...
    /// B folds: must refresh _pool_meta on the miss and ACCEPT, not fail closed.
    Gc gc(store_b, UInt128(1));
    const RebuildReport rep = gc.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rep.performed) << rep.refusal;           /// would throw CORRUPTED_DATA without refresh-on-miss
    EXPECT_TRUE(store_b->isAlgoAdmitted(BlobHashAlgo::Sha256));   /// cache unioned
}

TEST(CasPluggableHash, ForeignAlgoSegmentIsDebrisNotOurs)      /// spec §9.4 half
{
    /// plant an object under blobs/md5/aa/<32hex> — sweep skips it, fsck counts it unaccounted, and
    /// a 2-algo pool's OWN blobs under blobs/ch128 and blobs/sha256 are both classified.
}
```
(Fill the publish/fsck plumbing from the file's existing helpers; the assertions above are the contract.)

- [ ] **Step 2: implement; delete the Task 3 TEMPORARY; grep `TEMPORARY(P3` → zero.**
- [ ] **Step 3: build + tests + full regression filter green.**
- [ ] **Step 4: commit** — `feat(cas): path-derived BlobRef in sweep/fsck; fold validates admission with refresh-on-miss (Phase 3 T5)` + trailers.

---

### Task 6: cross-cutting cruxes + the no-bare-digest grep gates

**Files:**
- Test: `src/Disks/tests/gtest_cas_pluggable_hash.cpp` (or a new `gtest_cas_mixed_algo.cpp` via `./tests/...` — a gtest file, no numbered stateless test needed)

- [ ] **Step 1: THE reclaim crux (spec §9.3):** in-memory pool; admit ch128 + sha256; write orphan blobs under BOTH algos (direct `backend->putIfAbsent` at `layout.blobKey(ref)` with a valid envelope, mirroring `Sha256BlobSeenByCondemnSweepAndFsckNotSilentlySkipped` from Phase 2); `rebuildBaseline(force)` + drive GC; assert BOTH subsets condemned and reclaimed (`previewDeletes` covers both refs; after graduation/forced delete the backend holds ZERO blob bodies of either algo; fsck clean). Comment: goes red if any settlement/sweep path silently narrows to one algo.
- [ ] **Step 2: same-digest-different-algo end-to-end (spec §9.5):** two blobs `ch128:X`/`xxh3:X` (same digest VALUE) both present with distinct bodies; assert distinct object keys, distinct `.meta`, distinct settlement rows (fold both, in-degree per ref), deleting ONE leaves the other readable.
- [ ] **Step 3: grep gates (run from repo root; each MUST return zero matches in `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`):**
  - `grep -rn "kSourceEdgeKeySchema128\|kSourceEdgeKeySchemaSha256\|sourceEdgeDigestLen\|sourceEdgeKeySchemaFor"` (old schemas gone)
  - `grep -rn "TEMPORARY(P3"` (no leftover staging)
  - `grep -rn "blob_hash_len"` (pool-wide width gone; manifest header field gone)
  - `grep -rn "DigestCodec(store->poolMeta\|DigestCodec(pool_meta\|DigestCodec(meta)"` (pool-scoped codec gone)
  - `grep -rnE "unordered_(set|map)<BlobDigest"` (no digest-keyed identity containers)
  - `grep -rn "checkBlobHashAlgoMatches"` returns only the relaxed admission function itself
  Record each command + its empty output in the report.
- [ ] **Step 4: full regression filter + `--gtest_filter='*MixedAlgo*:*PluggableHash*:*BlobRef*'` green.**
- [ ] **Step 5: commit** — `test(cas): mixed-algo cruxes — two-algo reclaim, same-digest distinctness, no-bare-digest gates (Phase 3 T6)` + trailers.

---

### Task 7 (CONTROLLER-EXECUTED — not for a primitive subagent): live validation

- [ ] rustfs e2e (mirrors the 2026-07-11 fail-close e2e, now 3-step per spec §9.1): create pool ch128 + insert → flip config to sha256 WITHOUT flag → server refuses to load the disk (exit 36, `BAD_ARGUMENTS`, message names `blob_hash_allow_new`), data intact after revert → flip WITH flag → admitted; insert; blobs under BOTH `blobs/ch128/` and `blobs/sha256/`; SELECT correct; DROP both tables → GC reclaims BOTH subsets to `physical_bytes=0`; fsck `dangling=0`.
- [ ] mid-switch chaos soak (spec §9.7): 20m phase-3, flip `ch128 → sha256` (with the flag) at ~50%, `dangling==0` at every checkpoint.
- [ ] TLA note: re-run the settlement model with two distinct blob atoms (`ch128:X` / `xxh3:X`) — confirm no hidden digest-equality assumption; no full re-gate (spec §8).

## Self-review

Spec coverage: §2 decree → T1 + T6 gates; §4 per-entry BlobRef → T2; §5 admission/flag/union/monotone-cache → T4 + T5; §6 schema 3 / deletions / blobShard / sentinel guard → T3; §7 path-derived + foreign + render → T2 (inspect) + T5; §9 tests: 9.1→T4+T7, 9.2→T2, 9.3→T6, 9.3a→T3, 9.4→T5, 9.5→T6, 9.6→every task's regression filter, 9.7→T7, 9.8→T5, 9.9→T2, 9.10→T3; §12.2 seek fix — already landed (`035edbcf7e1`). Types/signatures consistent across tasks (`BlobRef`, `codecFor`, `SourceEdgeKeyCodec` static, `algos_used` vector<uint8_t>, `isAlgoAdmitted/refreshAdmittedAlgos`). Two explicitly-named TEMPORARY bridges (T2→T3, T3→T5), both grep-gated to zero in T6 — everything else lands with its subsystem, no legacy forms survive.
