#include "config.h"

#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

/// Phase 2 token-diff discovery: `supportsListTokens` capability probe.
///
/// Backends that can surface a per-key incarnation token through `list` return TRUE so that
/// GC `discover` may skip an unchanged root-shard body read when a listed token equals a
/// persisted folded token. Backends that cannot must return FALSE so discovery falls closed
/// to body reads.

namespace
{

/// Minimal stub backend that overrides every pure virtual with no-op implementations and
/// explicitly returns FALSE from `supportsListTokens`. Used to verify the default-false path.
class NoListTokenBackend final : public Backend
{
public:
    bool supportsListTokens() const override { return false; }

    std::optional<GetResult> get(const String & /*key*/, Range /*range*/ = {}) override
    {
        return std::nullopt;
    }

    HeadResult head(const String & /*key*/) override
    {
        return HeadResult{};
    }

    PutResult putIfAbsent(const String & /*key*/, const String & /*bytes*/, const ObjectMeta & /*meta*/ = {}) override
    {
        return PutResult{PutOutcome::Done, Token{}};
    }

    WriteSinkPtr putIfAbsentStream(const String & /*key*/, const ObjectMeta & /*meta*/ = {}) override
    {
        return nullptr;
    }

    PutResult putOverwrite(const String & /*key*/, const String & /*bytes*/, const Token & /*expected*/,
                           const ObjectMeta & /*meta*/ = {}) override
    {
        return PutResult{PutOutcome::Done, Token{}};
    }

    CasResult casPut(const String & /*key*/, const String & /*bytes*/, const std::optional<Token> & /*expected*/,
                     const ObjectMeta & /*meta*/ = {}) override
    {
        return CasResult{CasOutcome::Committed, Token{}};
    }

    DeleteOutcome deleteExact(const String & /*key*/, const Token & /*token*/) override
    {
        return DeleteOutcome{};
    }

    ListPage list(const String & /*prefix*/, const String & /*cursor*/, size_t /*limit*/) override
    {
        return ListPage{};
    }
};

} // namespace

/// The in-memory backend mints a monotonic token it surfaces through `list` — TRUE is correct.
TEST(CasBackendListTokens, InMemorySupportsListTokens)
{
    InMemoryBackend backend;
    EXPECT_TRUE(backend.supportsListTokens());
}

/// A backend explicitly overriding to FALSE must be observable as FALSE.
TEST(CasBackendListTokens, OverridableToFalse)
{
    NoListTokenBackend backend;
    EXPECT_FALSE(backend.supportsListTokens());
}

/// Characterization test: `CasFoldSeal::per_ns_shard` entries carry `folded_token` and
/// `folded_cursor` through the `encodeFoldSeal`/`decodeFoldSeal` codec byte-stably.
///
/// A later `discover` reads `ShardCoverage.folded_token`/`folded_cursor` back out of the
/// persisted `CasFoldSeal` to decide whether to skip a shard's body read. This test confirms
/// the round-trip was already correct in Phase 1d — no codec change was required.
TEST(CasShardCoverageRoundTrip, FoldedTokenAndCursorSurviveEncodeDecode)
{
    CasFoldSeal in;
    in.generation = 5;
    in.parent_generation = 4;

    /// One shard with a non-zero `folded_token`, non-zero `folded_cursor`, and a concrete
    /// `classification` value (2 = Folded per the spec enum).
    ShardCoverage cov;
    cov.classification = 2;
    cov.folded_token = Token{"etag-abc123", TokenType::ETag};
    cov.folded_cursor = 99;
    in.per_ns_shard["myns/7"] = cov;

    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));

    ASSERT_EQ(out.per_ns_shard.size(), 1u);
    const ShardCoverage & decoded = out.per_ns_shard.at("myns/7");
    EXPECT_EQ(decoded.classification, cov.classification);
    EXPECT_EQ(decoded.folded_token, cov.folded_token);
    EXPECT_EQ(decoded.folded_cursor, cov.folded_cursor);
    /// The full struct equality confirms no other field was corrupted.
    EXPECT_EQ(out, in);
}

/// ---- Phase 2 Task 4: discover skips a root shard iff listed token == persisted folded token ----

namespace
{

const UInt128 kGcD = hexToU128("0000000000000000000000000000000d");

/// Construct a single-shard Store over an in-memory backend and return both.
std::pair<StorePtr, std::shared_ptr<InMemoryBackend>> openDiscoveryStore()
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    return {store, backend};
}

ManifestRef discRef(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_instance_id = "srv-d:1", .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
}

} // anonymous namespace

/// After ONE full round with a committed publish, the fold seal carries a non-empty `folded_token`
/// for the shard. With no new publish the shard's root token has not advanced, so
/// `discoverDecisionsForTest()` must return Skip for that shard.
TEST(CasGcDiscovery, SkipsUnchangedShardWhenListedTokenEqualsFoldedToken)
{
    auto [store, backend] = openDiscoveryStore();
    const RootNamespace ns{"00/aa@disc@"};
    const ManifestRef r = discRef(1, 0xD1);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGcD);
    /// Run ONE full round so the fold seal is written with the shard's `folded_token`.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// No new publish: the shard's root manifest token has not changed since the seal.
    /// The seam must decide Skip for the shard.
    const std::map<String, Gc::DiscoverDecision> decisions = gc.discoverDecisionsForTest();

    const String shard_key = cursorKey(ns, 0);
    ASSERT_GT(decisions.count(shard_key), 0u) << "shard must appear in decisions";
    EXPECT_EQ(decisions.at(shard_key), Gc::DiscoverDecision::Skip)
        << "shard token unchanged since last fold seal => Skip";
}

/// After round 1 seals the `folded_token`, a NEW publish into the same shard advances the root
/// manifest's token past what was sealed. The seam must decide Read for that shard.
TEST(CasGcDiscovery, ReadsShardWhenTokenAdvancedOrMissing)
{
    auto [store, backend] = openDiscoveryStore();
    const RootNamespace ns{"00/aa@disc@"};
    const ManifestRef r1 = discRef(1, 0xD1);
    const ManifestRef r2 = discRef(2, 0xD2);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGcD);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// New publish: advances the shard's root manifest token beyond the sealed `folded_token`.
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);

    const std::map<String, Gc::DiscoverDecision> decisions = gc.discoverDecisionsForTest();

    /// At least one shard in the namespace must have a Read decision (the one that was published to).
    bool any_read = false;
    for (const auto & [key, decision] : decisions)
        if (decision == Gc::DiscoverDecision::Read)
            any_read = true;

    EXPECT_TRUE(any_read) << "a publish advanced the shard token past the fold seal => at least one Read";
}

/// Registry authority: two namespaces registered, publish into only one. Both namespaces must
/// appear in the decisions map — the registry universe is never shrunk by LIST.
TEST(CasGcDiscovery, RegistryUniverseNeverShrunkByList)
{
    auto [store, backend] = openDiscoveryStore();
    const RootNamespace ns1{"00/aa@disc@"};
    const RootNamespace ns2{"00/bb@disc@"};
    const ManifestRef r = discRef(1, 0xD1);

    /// Register ns2 into the registry WITHOUT publishing any shard manifest — so its shard is absent
    /// in storage (list won't see it), but the registry lists it.
    registerNamespaceRaw(*backend, store->layout(), ns2);

    /// Publish into ns1 so it has a real shard.
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns1, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns1, "tbl", std::nullopt, r);

    Gc gc(store, kGcD);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const std::map<String, Gc::DiscoverDecision> decisions = gc.discoverDecisionsForTest();

    /// Both namespaces must appear (registry universe, not list).
    const String key1 = cursorKey(ns1, 0);
    const String key2 = cursorKey(ns2, 0);
    EXPECT_GT(decisions.count(key1), 0u) << "ns1 (published) must appear in decisions";
    EXPECT_GT(decisions.count(key2), 0u) << "ns2 (registry-only, no shard body) must appear in decisions";
}
