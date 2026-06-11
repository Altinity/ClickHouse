#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>

#include <Common/Exception.h>

#include <base/hex.h>
#include <city.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
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

/// CityHash128 of bytes, composed into the canonical lowercase-hex id (identical composition to
/// `Cas::treeIdFor`, via `getHexUIntLowercase` over the cityhash `uint128`).
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

/// Write a Tree object: a NATURAL-length (no pad) Tree envelope followed by the canonical tree payload,
/// keyed by the tree id. Mirrors what Build::putTree will emit (Task 11).
inline DB::Cas::TreeId writeTreeRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    std::vector<DB::Cas::TreeEntry> entries, const DB::UInt128 & domain_id)
{
    const String encoded = DB::Cas::encodeTree(std::move(entries));
    const DB::Cas::TreeId id = DB::Cas::treeIdFor(encoded);

    DB::Cas::EnvelopeHeader header;
    header.kind = DB::Cas::ObjectKind::Tree;
    header.hash_algo = 1;
    header.logical_size = encoded.size();
    header.logical_hash = DB::Cas::hexToU128(id.string());
    header.domain_id = domain_id;
    header.incarnation_tag = DB::UInt128(0x1234);
    header.build_id = DB::UInt128(0x5678);

    const String head = DB::Cas::encodeEnvelopeHeader(header);
    backend.putIfAbsent(layout.treeKey(id), head + encoded);
    return id;
}

/// Publish a root-shard manifest fresh (create-if-absent CAS). One fresh publish per shard suffices for
/// the read-side tests; lifecycle tests that layer go through the Store CAS loop instead.
inline void publishRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, uint64_t shard, const DB::Cas::RootShard & root)
{
    backend.casPut(layout.rootShardKey(ns, shard), DB::Cas::encodeRootShard(root), /*expected*/ std::nullopt);
}

/// Inject GC state so a fresh `Store::open` over the same backend sees the given incarnations as
/// condemned. Writes `gc/state` ({round, fence_seq}) and one retired-set object at
/// `retiredKey(round, fence_seq, shard)`. The Store refreshes its `retireView` only at open, so the
/// caller injects BEFORE opening the Store whose Build will consult the view.
inline void injectRetire(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    uint64_t round, uint64_t fence_seq, uint64_t shard, std::vector<DB::Cas::RetiredEntry> entries)
{
    const String state = DB::Cas::encodeGcState(DB::Cas::GcState{.round = round, .fence_seq = fence_seq});
    const DB::Cas::HeadResult head = backend.head(layout.gcStateKey());
    if (!head.exists)
        backend.putIfAbsent(layout.gcStateKey(), state);
    else
        backend.putOverwrite(layout.gcStateKey(), state, head.token);

    backend.putIfAbsent(layout.retiredKey(round, fence_seq, shard),
        DB::Cas::encodeRetiredSet(DB::Cas::RetiredSet{.entries = std::move(entries)}));
}

/// Raise the `fence_round` of every shard of a namespace to at least `round`, exactly as a GC leader's
/// fence step (R3) does. For each shard 0..n_shards-1: read the manifest raw (decodeRootShard) if
/// present, `fence_round = max(fence_round, round)`, re-encode, and `casPut` it back against the
/// observed token. An ABSENT shard is created fresh holding only `fence_round = round` (mirrors GC
/// fencing a never-published shard) via `casPut(expected = nullopt)`.
inline void fenceNamespace(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, uint64_t n_shards, uint64_t round)
{
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
inline DB::Cas::Token displaceBlobToken(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const DB::Cas::BlobId & id)
{
    const String key = layout.blobKey(id);
    const auto got = backend.get(key);
    if (!got)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "displaceBlobToken: blob {} absent", key);

    DB::Cas::EnvelopeHeader header =
        DB::Cas::decodeEnvelopeHeader(got->bytes, got->bytes.size(), DB::Cas::ObjectKind::Blob);
    /// A fresh, distinct incarnation_tag forces a distinct body so the displaced token differs.
    header.incarnation_tag = header.incarnation_tag + DB::UInt128(1);
    header.pad_to_header_len = header.header_len;   /// preserve the exact header length on re-encode
    const String new_head = DB::Cas::encodeEnvelopeHeader(header);
    const String body = new_head + got->bytes.substr(header.header_len);

    DB::Cas::Token new_tok;
    backend.putOverwrite(key, body, got->token, &new_tok);
    return new_tok;
}

/// Duplicate of `Store::shardOf` (CityHash64(ref) % root_shards) for placing manifests in tests, since
/// shardOf is private and the Store API must not be widened for tests.
/// MUST match Store::shardOf exactly.
inline uint64_t shardOfForTest(const String & ref_name, uint64_t root_shards)
{
    return CityHash_v1_0_2::CityHash64(ref_name.data(), ref_name.size()) % root_shards;
}

}
