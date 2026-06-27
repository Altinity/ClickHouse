#include "config.h"

#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>

using namespace DB::Cas;

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
