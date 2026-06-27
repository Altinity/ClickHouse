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

namespace
{

const UInt128 kGcDiscovery = hexToU128("00000000000000000000000000000001");

ManifestRef discoveryRef(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_instance_id = "srv-a:1", .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
}

} // namespace

/// Phase-2 token-diff discover skips a QUIESCED root shard.
///
/// A shard becomes skippable only AFTER a settling round. The reason is architectural: the GC round's
/// `fence` step calls `mutateShard` on EVERY shard EVERY round (a per-shard fence marker), and `trim`
/// also `mutateShard`s a shard that had trimmable events. Both bump the shard's backend token — the
/// very token `Backend::list` surfaces. `recheck` records `folded_token` from the POST-FENCE shard
/// token it reads via `readShard`. So:
///   - Round 1 folds AND trims the freshly-published events; recheck records the post-fence token, but
///     trim then bumps the token again => next discover sees an advanced token => Read.
///   - Round 2 has nothing to trim (the published events are gone); recheck records the post-fence
///     token, which is now the shard's FINAL token for the round => next discover sees an unchanged
///     token => Skip.
/// This 1-round settle is the intended, safe conservative behavior (the optimal one-round skip is
/// deferred to the backlog — it would require capturing the post-trim token, a model extension).
TEST(CasGcDiscovery, SkipsQuiescedShard)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"srv1/tbl"};
    const ManifestRef r = discoveryRef(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGcDiscovery);

    /// Round 1: folds + trims the published events. After it, trim's mutation advanced the token past
    /// the post-fence token recheck recorded — so the shard is NOT yet skippable.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// Round 2: a settling round — nothing left to trim. recheck records the post-fence token, which is
    /// the shard's final token for the round (no later mutation), so the NEXT discover sees it unchanged.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const auto decisions = gc.discoverDecisionsForTest();
    const String ck = cursorKey(ns, 0);
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Skip)
        << "a quiesced shard must be skipped after a settling round (token unchanged since recheck)";
}

/// Phase-2 token-diff discover READS a shard whose token advanced (a new publish bumps it) — the safe
/// direction. After the shard has quiesced (skippable), a fresh committed publish advances the shard's
/// backend token; the listed token no longer equals the sealed `folded_token`, so the shard is Read.
TEST(CasGcDiscovery, ReadsShardWhenTokenAdvancedOrMissing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"srv1/tbl"};
    const ManifestRef r1 = discoveryRef(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGcDiscovery);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// round 1: fold + trim
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// round 2: settle => shard now skippable

    const String ck = cursorKey(ns, 0);
    ASSERT_EQ(gc.discoverDecisionsForTest().at(ck), Gc::DiscoverDecision::Skip)
        << "precondition: the shard quiesced and is skippable";

    /// A NEW committed publish advances the shard's backend token past the sealed folded_token.
    const ManifestRef r2 = discoveryRef(2, 0xBB);
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl2", std::nullopt, r2);

    const auto decisions = gc.discoverDecisionsForTest();
    ASSERT_TRUE(decisions.contains(ck));
    EXPECT_EQ(decisions.at(ck), Gc::DiscoverDecision::Read)
        << "a shard whose token advanced since the fold must be read (fail closed in the safe direction)";
}

/// Phase-2 token-diff discover universe is the REGISTRY universe, never shrunk by LIST. Two namespaces
/// are registered but a publish lands in only ONE; both must still appear in the decisions (a namespace
/// with no LIST-visible shard token defaults to Read — fail closed — but is never DROPPED from the set).
TEST(CasGcDiscovery, RegistryUniverseNeverShrunkByList)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns_a{"srv1/tbl_a"};
    const RootNamespace ns_b{"srv1/tbl_b"};

    /// Register BOTH namespaces (a publish registers its own; register the other explicitly).
    registerNamespaceRaw(*backend, store->layout(), ns_b);

    const ManifestRef r = discoveryRef(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns_a, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns_a, "tbl", std::nullopt, r);

    Gc gc(store, kGcDiscovery);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const auto decisions = gc.discoverDecisionsForTest();
    EXPECT_TRUE(decisions.contains(cursorKey(ns_a, 0))) << "the published namespace must be present";
    EXPECT_TRUE(decisions.contains(cursorKey(ns_b, 0)))
        << "a registered-but-empty namespace must remain in the universe (registry authority, not LIST)";
}
