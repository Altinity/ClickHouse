#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>

using namespace DB::Cas;

/// The GC-reclaim tests that used to live here (`AbandonedPrecommitOrphansManifestUntilFix`,
/// `ReclaimIsIdempotentAndSelfTerminating`, `SkipPreservedForLivePrecommitAndForNoPrecommit`,
/// `DoubleRemovalOfReclaimedPrecommitIsIdempotent`) were removed with the snapshot+log ref model.
/// They asserted that GC reclaims an abandoned precommit once the mount watermark proves it dead, and
/// that the token-diff Skip optimization self-terminates. Per spec §Responsibility Boundary, reclaiming
/// an abandoned precommit is now the WRITER's job (it appends the exact `owner_transition` removal), and
/// the token-diff Skip machinery (`computeDiscoverDecisions`/`discoverDecisionsForTest`) no longer exists
/// -- the "did it change" signal is simply logs above the durable cursor. There is no GC-side reclaim to
/// assert, so these tests are obsolete rather than adaptable.

/// The one still-meaningful assertion: `ShardCoverage.has_live_precommit` / `min_live_precommit_*`
/// survive as fold-seal fields, so their codec round-trip is pinned here.
TEST(CasDanglingPrecommit, ShardCoverageRoundTripsMinLivePrecommit)
{
    CasFoldSeal seal;
    seal.generation = 3;
    seal.parent_generation = 2;
    ShardCoverage cov;
    cov.classification = 1;
    cov.folded_cursor = 7;
    cov.has_live_precommit = true;
    cov.min_live_precommit_writer_epoch = 1;
    cov.min_live_precommit_build_sequence = 5;
    seal.per_ns_shard["srv/tbl@cas@/0"] = cov;

    const CasFoldSeal back = decodeFoldSeal(encodeFoldSeal(seal));
    const ShardCoverage & r = back.per_ns_shard.at("srv/tbl@cas@/0");
    EXPECT_TRUE(r.has_live_precommit);
    EXPECT_EQ(r.min_live_precommit_writer_epoch, 1u);
    EXPECT_EQ(r.min_live_precommit_build_sequence, 5u);

    /// Default (no live precommit) round-trips as absent.
    CasFoldSeal empty_seal;
    empty_seal.per_ns_shard["srv/tbl@cas@/1"] = ShardCoverage{};
    const CasFoldSeal e_back = decodeFoldSeal(encodeFoldSeal(empty_seal));
    EXPECT_FALSE(e_back.per_ns_shard.at("srv/tbl@cas@/1").has_live_precommit);
}
