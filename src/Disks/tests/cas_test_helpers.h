#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Common/Exception.h>

#include <base/hex.h>
#include <city.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace DB::Cas::tests
{

/// Run `fn`, expect a DB::Exception with EXACTLY `expected_code` (CORRUPTED_DATA-vs-NOT_IMPLEMENTED
/// is part of the fail-closed contract: an unknown future format must be NOT_IMPLEMENTED, never
/// misreported as corruption).
template <typename F>
void expectThrowsCode(int expected_code, F && fn)
{
    try
    {
        fn();
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), expected_code);
    }
}

/// Build a `LocalObjectStorage` rooted at a fresh, unique temporary directory (one per call).
///
/// Used by the unit tests that exercise the `Cas::Backend` seam against a real on-disk object storage
/// (the `EmulatedSingleProcess` adapter mode and the capability probe). The construction mirrors the
/// existing PoC gtest `gtest_content_addressed_metadata.cpp`: for `LocalObjectStorage` the object key
/// IS the local path verbatim, so the unique root keeps every test instance isolated even under the
/// parallel gtest runner.
inline DB::ObjectStoragePtr makeLocalObjectStorageForTest()
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_unit_" + unique)).string();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    DB::LocalObjectStorageSettings settings("test", root, /*read_only_=*/false);
    return std::make_shared<DB::LocalObjectStorage>(std::move(settings));
}

/// ---- on-storage write fixtures (shared by the Store read/lifecycle/build tests, Tasks 9-13) ----
///
/// These produce objects through the SAME codecs the Store reads — the documented on-storage
/// interface, not white-box pokes — so a test asserts a real round trip across the format boundary.

/// CityHash128 of bytes, composed into the canonical lowercase-hex id.
inline String hexOf(const String & bytes)
{
    return getHexUIntLowercase(CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size()));
}

/// The content id of `bytes` as a UInt128 — definitionally consistent with `idOf` (parses the same hex).
inline DB::UInt128 u128Of(const String & bytes)
{
    return DB::Cas::hexToU128(hexOf(bytes));
}

/// The content id of `bytes` as a strong BlobId.
inline DB::Cas::BlobId idOf(const String & bytes)
{
    return DB::Cas::BlobId(hexOf(bytes));
}

/// Write a Blob object: a fixed-length (pad_to_header_len = blob_header_len) envelope followed by the
/// raw payload, keyed by content. Mirrors what Build::putBlob will emit (Task 11).
inline DB::Cas::BlobId writeBlobRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const String & payload,
    uint64_t blob_header_len, const DB::UInt128 & domain_id)
{
    const DB::Cas::BlobId id = idOf(payload);

    DB::Cas::EnvelopeHeader header;
    header.kind = DB::Cas::ObjectKind::Blob;
    header.hash_algo = 1;
    header.logical_size = payload.size();
    header.logical_hash = u128Of(payload);
    header.domain_id = domain_id;
    header.incarnation_tag = DB::UInt128(0x1234);
    header.build_id = DB::UInt128(0x5678);
    header.pad_to_header_len = static_cast<uint32_t>(blob_header_len);

    const String head = DB::Cas::encodeEnvelopeHeader(header);
    backend.putIfAbsent(layout.blobKey(id), head + payload);
    return id;
}

/// Forward declaration: `appendOwnerEvent` (below) calls `registerNamespaceRaw`, which after Task 4
/// is a no-op (LIST-based discovery needs no explicit registration) defined further down.
inline void registerNamespaceRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::RootNamespace & ns);

/// Write a part-manifest body object directly via the manifest codec, exactly as Build::stageManifest
/// emits it. Returns the ManifestId. Used by GC fold/retire/fsck tests to stage owner targets.
inline DB::Cas::ManifestId writeManifestRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, const DB::Cas::ManifestRef & ref,
    const std::vector<DB::Cas::ManifestEntry> & entries)
{
    const DB::Cas::ManifestId id{ns, ref};
    DB::Cas::PartManifest body;
    body.ref = ref;
    body.root_namespace_id = ns;
    body.entries = entries;
    body.payload_digest = DB::Cas::computePayloadDigest(body);
    backend.putIfAbsent(layout.manifestKey(id), DB::Cas::encodePartManifest(body));
    return id;
}

/// A blob ManifestEntry referencing `hash` at `path` (size 1, the GC fold counts edges, not bytes).
inline DB::Cas::ManifestEntry blobEntryFor(const String & path, const DB::UInt128 & hash, uint64_t size = 1)
{
    DB::Cas::ManifestEntry e;
    e.path = path;
    e.placement = DB::Cas::EntryPlacement::Blob;
    e.blob_hash = hash;
    e.blob_size = size;
    return e;
}

/// Append ONE RootOwnerEvent to a shard's journal via read-modify-CAS (mirroring the production Build
/// path): bump shard_version, append the event at the new version, maintain `refs` for committed
/// bindings, and CAS. Returns the event's transition_version. Registers the namespace first.
inline uint64_t appendOwnerEvent(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, uint64_t shard,
    std::optional<DB::Cas::OwnerBinding> old_binding,
    std::optional<DB::Cas::OwnerBinding> new_binding,
    const String & committed_ref_name = {})
{
    registerNamespaceRaw(backend, layout, ns);
    const String key = layout.rootShardKey(ns, shard);
    while (true)
    {
        const auto got = backend.get(key);
        DB::Cas::RootShard root;
        std::optional<DB::Cas::Token> expected;
        if (got)
        {
            root = DB::Cas::decodeRootShard(got->bytes);
            expected = got->token;
        }
        const uint64_t version = root.shard_version + 1;
        root.shard_version = version;
        root.journal.push_back(DB::Cas::RootOwnerEvent{
            .transition_version = version, .old_binding = old_binding, .new_binding = new_binding});

        /// Maintain committed refs so the read path + fsck see the current owner.
        if (new_binding && new_binding->owner_kind == DB::Cas::OwnerKind::Committed && !committed_ref_name.empty())
        {
            DB::Cas::RootRef r;
            r.ref_name = committed_ref_name;
            r.manifest_ref = new_binding->manifest_ref;
            root.refs[committed_ref_name] = r;
        }
        if ((!new_binding || new_binding->owner_kind != DB::Cas::OwnerKind::Committed)
            && old_binding && old_binding->owner_kind == DB::Cas::OwnerKind::Committed && !committed_ref_name.empty())
            root.refs.erase(committed_ref_name);

        const auto outcome = backend.casPut(key, DB::Cas::encodeRootShard(root), expected).outcome;
        if (outcome == DB::Cas::CasOutcome::Committed)
            return version;
    }
}

/// Publish a committed ref over `ref` (old none unless `old_ref` set). Returns the event version.
inline uint64_t publishCommittedTransition(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::RootNamespace & ns,
    const String & ref_name, std::optional<DB::Cas::ManifestRef> old_ref, const DB::Cas::ManifestRef & new_ref,
    uint64_t shard = 0)
{
    std::optional<DB::Cas::OwnerBinding> old_b;
    if (old_ref)
        old_b = DB::Cas::OwnerBinding{.owner_kind = DB::Cas::OwnerKind::Committed,
            .ref_name = ref_name, .build_id = {}, .manifest_ref = *old_ref};
    DB::Cas::OwnerBinding new_b{.owner_kind = DB::Cas::OwnerKind::Committed,
        .ref_name = ref_name, .build_id = {}, .manifest_ref = new_ref};
    return appendOwnerEvent(backend, layout, ns, shard, old_b, new_b, ref_name);
}

/// Drop a committed ref (old committed / new none). Returns the event version.
inline uint64_t dropRefTransition(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::RootNamespace & ns,
    const String & ref_name, const DB::Cas::ManifestRef & old_ref, uint64_t shard = 0)
{
    DB::Cas::OwnerBinding old_b{.owner_kind = DB::Cas::OwnerKind::Committed,
        .ref_name = ref_name, .build_id = {}, .manifest_ref = old_ref};
    return appendOwnerEvent(backend, layout, ns, shard, old_b, std::nullopt, ref_name);
}

/// Add a precommit binding (old none / new precommit). Returns the event version.
inline uint64_t addPrecommitTransition(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::RootNamespace & ns,
    const DB::UInt128 & build_id, const String & final_ref_name, std::optional<DB::Cas::ManifestRef> old_ref,
    const DB::Cas::ManifestRef & new_ref, uint64_t shard = 0)
{
    std::optional<DB::Cas::OwnerBinding> old_b;
    if (old_ref)
        old_b = DB::Cas::OwnerBinding{.owner_kind = DB::Cas::OwnerKind::Committed,
            .ref_name = final_ref_name, .build_id = {}, .manifest_ref = *old_ref};
    DB::Cas::OwnerBinding new_b{.owner_kind = DB::Cas::OwnerKind::Precommit,
        .ref_name = final_ref_name, .build_id = build_id, .manifest_ref = new_ref};
    return appendOwnerEvent(backend, layout, ns, shard, old_b, new_b);
}

/// Promote a precommit to committed at the SAME manifest_ref (an owner move: equal old/new). Returns
/// the event version.
inline uint64_t promoteTransition(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::RootNamespace & ns,
    const DB::UInt128 & build_id, const String & final_ref_name, const DB::Cas::ManifestRef & ref,
    uint64_t shard = 0)
{
    DB::Cas::OwnerBinding old_b{.owner_kind = DB::Cas::OwnerKind::Precommit,
        .ref_name = final_ref_name, .build_id = build_id, .manifest_ref = ref};
    DB::Cas::OwnerBinding new_b{.owner_kind = DB::Cas::OwnerKind::Committed,
        .ref_name = final_ref_name, .build_id = {}, .manifest_ref = ref};
    return appendOwnerEvent(backend, layout, ns, shard, old_b, new_b, final_ref_name);
}

/// Exact-token delete of a manifest body (HEAD then deleteExact). No-op when absent.
inline void deleteManifestBody(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::ManifestId & id)
{
    const String key = layout.manifestKey(id);
    const DB::Cas::HeadResult h = backend.head(key);
    if (h.exists)
        backend.deleteExact(key, h.token);
}

/// Formerly wrote the namespace into `gc/registry` for GC discovery; after Task 4 discovery
/// is LIST-based and a namespace is visible once any ref shard exists — no explicit registration.
inline void registerNamespaceRaw(
    DB::Cas::Backend & /*backend*/, const DB::Cas::Layout & /*layout*/, const DB::Cas::RootNamespace & /*ns*/)
{
    /// No-op: Task 4 deleted the registry; LIST(cas/refs/) is now the discovery authority.
}

/// Publish a root-shard manifest fresh (create-if-absent CAS). One fresh publish per shard suffices for
/// the read-side tests; lifecycle tests that layer go through the Store CAS loop instead.
/// Registers the namespace first (W-REGISTER) so GC discovery sees it.
inline void publishRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, uint64_t shard, const DB::Cas::RootShard & root)
{
    registerNamespaceRaw(backend, layout, ns);
    backend.casPut(layout.rootShardKey(ns, shard), DB::Cas::encodeRootShard(root), /*expected*/ std::nullopt);
}

/// Encode a CAGS document carrying only {round, fence_seq} — everything else defaulted. The
/// retire-view tests and `injectRetire` only care about these two fields.
inline String encodeMinimalGcState(uint64_t round, uint64_t fence_seq)
{
    DB::Cas::GcState state;
    state.round = round;
    state.fence_seq = fence_seq;
    return DB::Cas::encodeGcState(state);
}

/// Inject GC state so a fresh `Store::open` over the same backend sees the given incarnations as
/// condemned. Writes one CURRENT retired-set object and records its key in `gc/state.retired_refs[shard]`
/// — the on-storage interface `RetireView::refresh` dereferences (ack-floor redesign: refs, not a LIST).
/// The Store refreshes its `retireView` at open (and each beat), so the caller injects BEFORE opening the
/// Store whose Build consults the view.
///
/// ACK-FLOOR: entries carry a `condemn_round` (and optionally `delete_pending`) — the caller supplies
/// them fully-formed in `entries` (default-constructed entries have condemn_round 0, delete_pending false).
/// The set is written under the reserved round component 0 (no-real-round sentinel), keyed
/// retiredKey(0, 0, 0, shard); the minimal injected gc/state has snap_generation == snap_attempt == 0. The
/// call sites' semantics are unchanged: a condemned token the writer publish gate must see after refresh.
inline void injectRetire(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    uint64_t round, uint64_t fence_seq, uint64_t shard, std::vector<DB::Cas::RetiredEntry> entries)
{
    const String key = layout.retiredKey(/*generation*/0, /*attempt*/0, /*round*/0, shard);
    backend.putIfAbsent(key,
        DB::Cas::encodeRetiredSet(DB::Cas::RetiredSet{.entries = std::move(entries)}));

    /// gc/state carries {round, fence_seq} AND retired_refs[shard] = key: RetireView reads the current
    /// retired list by dereferencing retired_refs (ack-floor redesign), so the ref must be recorded for
    /// the injected set to be visible to the writer publish gate. Read-modify-CAS so multiple injectRetire
    /// calls for different shards accumulate refs rather than clobbering.
    DB::Cas::GcState gc_state;
    const DB::Cas::HeadResult head = backend.head(layout.gcStateKey());
    if (head.exists)
        gc_state = DB::Cas::decodeGcState(backend.get(layout.gcStateKey())->bytes);
    gc_state.round = round;
    gc_state.fence_seq = fence_seq;
    gc_state.retired_refs[shard] = key;
    const String state = DB::Cas::encodeGcState(gc_state);
    if (!head.exists)
        backend.putIfAbsent(layout.gcStateKey(), state);
    else
        backend.putOverwrite(layout.gcStateKey(), state, head.token);
}

/// Whether blob `hash` is absent from the backend (its exact-token content object is gone).
inline bool blobAbsent(DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::UInt128 & hash)
{
    return !backend.head(layout.blobKey(DB::Cas::BlobId(DB::Cas::u128ToHex(hash)))).exists;
}

/// ACK-FLOOR reclaim loop (the canonical pipeline driver): run regular rounds, advancing the store's own
/// mount ack after each round (`renewWatermarkOnce` runs the beat, so `observed_gc_round` follows the
/// freshly-committed gc/state.round and the heartbeat floor advances). A blob condemned at round K is
/// deleted by round K+3 with acks kept current (condemn K -> pending at first pass whose min_ack > K ->
/// physical delete the pass after that). Returns true as soon as the blob became absent.
inline bool runRoundsUntilAbsent(
    const DB::Cas::StorePtr & store, DB::Cas::Gc & gc, DB::Cas::Backend & backend,
    const DB::Cas::Layout & layout, const DB::UInt128 & hash, int max_rounds = 8)
{
    for (int i = 0; i < max_rounds; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
        if (blobAbsent(backend, layout, hash))
            return true;
    }
    return blobAbsent(backend, layout, hash);
}

/// The decoded CURRENT retired set for `shard` (dereferenced through gc/state.retired_refs). Empty when
/// gc/state or the ref is absent. Used by ack-floor tests to assert an entry's pending/condemn state.
inline DB::Cas::RetiredSet currentRetiredSet(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, uint64_t shard)
{
    const auto st = backend.get(layout.gcStateKey());
    if (!st)
        return {};
    const DB::Cas::GcState gc_state = DB::Cas::decodeGcState(st->bytes);
    const auto it = gc_state.retired_refs.find(shard);
    if (it == gc_state.retired_refs.end())
        return {};
    const auto got = backend.get(it->second);
    if (!got)
        return {};
    return DB::Cas::decodeRetiredSet(got->bytes);
}

/// Raise the `fence_round` of every shard of a namespace to at least `round`, exactly as the REMOVED
/// pre-redesign fence step (R3) did — kept to synthesize historical/birth-floor shard state. For each shard 0..n_shards-1: read the manifest raw (decodeRootShard) if
/// present, `fence_round = max(fence_round, round)`, re-encode, and `casPut` it back against the
/// observed token. An ABSENT shard is created fresh holding only `fence_round = round` (mirrors GC
/// fencing a never-published shard) via `casPut(expected = nullopt)`.
inline void fenceNamespace(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, uint64_t n_shards, uint64_t round)
{
    registerNamespaceRaw(backend, layout, ns);
    for (uint64_t shard = 0; shard < n_shards; ++shard)
    {
        const String key = layout.rootShardKey(ns, shard);
        const auto got = backend.get(key);
        if (got)
        {
            DB::Cas::RootShard root = DB::Cas::decodeRootShard(got->bytes);
            root.fence_round = std::max(root.fence_round, round);
            backend.casPut(key, DB::Cas::encodeRootShard(root), got->token);
        }
        else
        {
            DB::Cas::RootShard root;
            root.fence_round = round;
            backend.casPut(key, DB::Cas::encodeRootShard(root), /*expected*/ std::nullopt);
        }
    }
}

/// Displace a blob's incarnation out-of-band (as a racing writer would): GET it, mint a fresh
/// incarnation_tag in its envelope header (preserving header_len + payload), putOverwrite against the
/// current token, and return the NEW token. Used to drive the W-REVALIDATE adopt branch (current token
/// differs from the writer's stale observation).
inline DB::Cas::Token displaceObjectToken(
    DB::Cas::Backend & backend, const String & key, DB::Cas::ObjectKind kind)
{
    const auto got = backend.get(key);
    if (!got)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "displaceObjectToken: object {} absent", key);

    DB::Cas::EnvelopeHeader header =
        DB::Cas::decodeEnvelopeHeader(got->bytes, got->bytes.size(), kind);
    /// A fresh, distinct incarnation_tag forces a distinct body so the displaced token differs.
    header.incarnation_tag = header.incarnation_tag + DB::UInt128(1);
    header.pad_to_header_len = header.header_len;   /// preserve the exact header length on re-encode
    const String new_head = DB::Cas::encodeEnvelopeHeader(header);
    const String body = new_head + got->bytes.substr(header.header_len);

    return backend.putOverwrite(key, body, got->token).token;
}

inline DB::Cas::Token displaceBlobToken(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::BlobId & id)
{
    return displaceObjectToken(backend, layout.blobKey(id), DB::Cas::ObjectKind::Blob);
}

/// Duplicate of `Store::shardOf` (CityHash64(ref) % root_shards) for placing manifests in tests, since
/// shardOf is private and the Store API must not be widened for tests.
/// MUST match Store::shardOf exactly.
inline uint64_t shardOfForTest(const String & ref_name, uint64_t root_shards)
{
    return CityHash_v1_0_2::CityHash64(ref_name.data(), ref_name.size()) % root_shards;
}

/// ---- GC-core (Phase 1d) test helpers over the part-manifest model ----

/// Open a Store over `backend` with a single root shard (so cursor keys are "ns/0").
/// gc_trim_min_events=0 (eager, pre-B12 behaviour) is the test default so existing tests that
/// assert "the event was trimmed after one round" do not need to be updated.
/// New tests that want to exercise the lazy-trim threshold pass their own PoolConfig to Store::open.
inline DB::Cas::StorePtr openStoreForTest(std::shared_ptr<DB::Cas::InMemoryBackend> backend)
{
    return DB::Cas::Store::open(std::move(backend),
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0});
}

/// Write a blob object (envelope + payload) addressed by `hash`, so a HEAD returns a token. The bytes
/// are arbitrary (GC never reads them); the hash is what the manifest entry references.
inline void writeBlobBody(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::UInt128 & hash,
    uint64_t blob_header_len = 256)
{
    DB::Cas::EnvelopeHeader header;
    header.kind = DB::Cas::ObjectKind::Blob;
    header.hash_algo = 1;
    header.logical_size = 1;
    header.logical_hash = hash;
    header.domain_id = DB::UInt128(0x42);
    header.incarnation_tag = DB::UInt128(0x1234);
    header.build_id = DB::UInt128(0x5678);
    header.pad_to_header_len = static_cast<uint32_t>(blob_header_len);
    const String head = DB::Cas::encodeEnvelopeHeader(header);
    backend.putIfAbsent(layout.blobKey(DB::Cas::BlobId(DB::Cas::u128ToHex(hash))), head + String("x"));
}

/// The latest GC generation (snap_generation pointer in gc/state), or 0 when absent.
inline uint64_t currentGenerationOf(DB::Cas::Backend & backend, const DB::Cas::Layout & layout)
{
    const auto got = backend.get(layout.gcStateKey());
    if (!got)
        return 0;
    return DB::Cas::decodeGcState(got->bytes).snap_generation;
}

/// The adopted attempt (snap_attempt pointer in gc/state), or 0 when absent.
inline uint64_t currentAttemptOf(DB::Cas::Backend & backend, const DB::Cas::Layout & layout)
{
    const auto got = backend.get(layout.gcStateKey());
    if (!got)
        return 0;
    return DB::Cas::decodeGcState(got->bytes).snap_attempt;
}

/// The in-degree of a blob in the current GC generation's sealed run (0 when absent/zeroed).
inline int64_t inDegreeOf(DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::UInt128 & hash)
{
    return DB::Cas::inDegreeInGeneration(backend, layout, currentGenerationOf(backend, layout),
                                         currentAttemptOf(backend, layout), /*shard*/0, hash);
}

/// The cursor key "ns/shard" — matches CasGcCursorKey::cursorKey.
inline String cursorKeyForTest(const DB::Cas::RootNamespace & ns, uint64_t shard)
{
    return ns.string() + "/" + std::to_string(shard);
}

/// The folded cursor sealed for (ns, shard) by the latest fold seal, or 0 when absent. After a COMPLETE
/// round the gc/state generation pointer is the recheck's COMPLETION generation (G+2 for a round started
/// at G), but the fold seal is written at the FOLD generation (G+1) — recheck writes a completion seal,
/// not a fold seal. So scan downward from the current generation for the most recent existing fold seal.
inline uint64_t foldCursorOf(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::RootNamespace & ns, uint64_t shard)
{
    const uint64_t gen = currentGenerationOf(backend, layout);
    const uint64_t attempt = currentAttemptOf(backend, layout);
    for (uint64_t g = gen; ; --g)
    {
        if (const auto got = backend.get(layout.foldSealKey(g, attempt)))
        {
            const DB::Cas::CasFoldSeal seal = DB::Cas::decodeFoldSeal(got->bytes);
            const auto it = seal.per_ns_shard.find(cursorKeyForTest(ns, shard));
            return it != seal.per_ns_shard.end() ? it->second.folded_cursor : 0;
        }
        if (g == 0)
            return 0;
    }
}

/// Set a server root's durable floor (so orphan-sweep eligibility can be driven). After the ack-floor
/// merge the floor rides the mount lease body (`mountKey`), so this seeds a MountLease carrying
/// `{writer_epoch, min_active}` — exactly what `prefixEligible` reads.
inline void setWatermarkMinActive(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const String & server_root_id,
    uint64_t writer_epoch, uint64_t min_active)
{
    DB::Cas::MountLease m;
    m.server_uuid = DB::UInt128(0);
    m.writer_epoch = writer_epoch;
    m.min_active = min_active;
    m.seq = 1;
    const String key = layout.mountKey(server_root_id);
    const DB::Cas::HeadResult h = backend.head(key);
    if (h.exists)
        backend.putOverwrite(key, DB::Cas::encodeMountLease(m), h.token);
    else
        backend.putIfAbsent(key, DB::Cas::encodeMountLease(m));
}

/// Counts head/get/putIfAbsent per key for op-count assertions (Pillar B / A1 tests).
class CountingBackend : public DB::Cas::InMemoryBackend
{
public:
    DB::Cas::HeadResult head(const String & key) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++head_counts[key];
            ++head_total;
        }
        return InMemoryBackend::head(key);
    }

    std::optional<DB::Cas::GetResult> get(const String & key, DB::Cas::Range range = {}) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++get_counts[key];
            ++get_total;
        }
        return InMemoryBackend::get(key, range);
    }

    DB::Cas::PutResult putIfAbsent(const String & key, const String & bytes, const DB::Cas::ObjectMeta & meta = {}) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++put_counts[key];
            ++put_total;
        }
        return InMemoryBackend::putIfAbsent(key, bytes, meta);
    }

    uint64_t headCount(const String & key) const { return lookup(head_counts, key); }
    uint64_t getCount(const String & key) const { return lookup(get_counts, key); }
    uint64_t putCount(const String & key) const { return lookup(put_counts, key); }
    uint64_t headTotal() const { std::lock_guard lock(count_mutex); return head_total; }
    uint64_t getTotal() const { std::lock_guard lock(count_mutex); return get_total; }
    uint64_t putTotal() const { std::lock_guard lock(count_mutex); return put_total; }

    void resetCounts()
    {
        std::lock_guard lock(count_mutex);
        head_counts.clear();
        get_counts.clear();
        put_counts.clear();
        head_total = get_total = put_total = 0;
    }

private:
    uint64_t lookup(const std::map<String, uint64_t> & m, const String & key) const
    {
        std::lock_guard lock(count_mutex);
        const auto it = m.find(key);
        return it == m.end() ? 0 : it->second;
    }

    mutable std::mutex count_mutex;
    std::map<String, uint64_t> head_counts;
    std::map<String, uint64_t> get_counts;
    std::map<String, uint64_t> put_counts;
    uint64_t head_total = 0;
    uint64_t get_total = 0;
    uint64_t put_total = 0;
};

}
